//! signal-host: the Signal backend of KolibriOS's messenger
//! (docs/messengers.md). presage (whisperfish's Signal client library, on
//! Signal's libsignal) as a linked device, its store in SQLite, speaking
//! the JSON lines of `yappari-wa -stdio` on standard input and output (the
//! same methods and signals where Yappari has them):
//!
//! ```text
//! in:  {"id": 1, "call": "Status", "args": []}
//! out: {"id": 1, "result": ["logged-out"]}            (or "connecting", "connected")
//! in:  {"id": 2, "call": "GetQR", "args": []}
//! out: {"signal": "QRCode", "args": ["sgnl://linkdevice?uuid=...&pub_key=..."]}
//!      {"id": 2, "result": ["sgnl://linkdevice?uuid=...&pub_key=..."]}
//! in:  {"id": 3, "call": "SendText", "args": ["", "<ACI uuid> or group:<hex master key>", "hello"]}
//! out: {"id": 3, "result": ["1761750000000"]}       (the message's id: its timestamp)
//!      {"signal": "Receipt", "args": ["<chat>", "", ["1761750000000"], "server", 1761750000]}
//! in:  {"id": 4, "call": "GetContacts", "args": []}
//! out: {"id": 4, "result": [[["<ACI uuid>", "<name>", ""], ...]]}
//! in:  {"id": 5, "call": "GetGroups", "args": []}
//! out: {"id": 5, "result": [[["group:<hex master key>", "<title>"], ...]]}
//! out: {"signal": "StateChanged", "args": ["connected"]}
//!      {"signal": "Message", "args": ["<timestamp ms>", "<chat>", "<sender>", "", 1761750000, false, "text", "hi", "", 0]}
//!      {"signal": "History", "args": [... as Message: the store's, at the start]}
//!      {"signal": "Receipt", "args": ["<chat>", "<sender>", ["<timestamp ms>"], "delivered" or "read", 1761750000]}
//!      {"signal": "ChatState", "args": ["<chat>", "<sender>", true]}
//!      {"signal": "Error", "args": ["..."]}
//! ```
//!
//! GetQR starts linking: Signal's server hands out a provisioning address
//! (the QR code the phone's Signal scans: Settings, Linked devices); when
//! the server closes it unscanned (after about 90 s), a new one follows
//! (another QRCode signal). Once linked, the messages are received and
//! stored; at a start with a linked store, the newest 40 of each contact's
//! and group's are given as History signals first. SendText also takes
//! (chat, text); its id argument is not used (a Signal message is known by
//! its timestamp). MarkRead(chat, sender, ids) is accepted and does
//! nothing yet (no read receipts sent). The program ends with its standard
//! input; its log goes to standard error (RUST_LOG, warnings by default).
//! The store: `$XDG_DATA_HOME/signal-host/store.db` (`$HOME/.local/share/...`).

use std::cell::RefCell;
use std::io::Write;
use std::rc::Rc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use futures::{channel::oneshot, future, pin_mut, StreamExt};
use presage::libsignal_service::configuration::SignalServers;
use presage::libsignal_service::content::{Content, ContentBody, DataMessage};
use presage::libsignal_service::prelude::Uuid;
use presage::libsignal_service::protocol::ServiceId;
use presage::manager::Registered;
use presage::model::identity::OnNewIdentity;
use presage::model::messages::Received;
use presage::proto::sync_message::{Content as SyncContent, Sent};
use presage::proto::{receipt_message, typing_message, ReceiptMessage, SyncMessage, TypingMessage};
use presage::store::{ContentsStore, Thread};
use presage::Manager;
use presage_store_sqlite::SqliteStore;
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::task::spawn_local;

const DEVICE_NAME: &str = "KolibriOS";

struct Host {
    state: &'static str,
    store: SqliteStore,
    linking: bool,
    qr: Option<String>,
    qr_waiting: Vec<Value>,
    manager: Option<Manager<SqliteStore, Registered>>,
}

type H = Rc<RefCell<Host>>;

fn out(v: Value) {
    let mut o = std::io::stdout().lock();
    let _ = writeln!(o, "{v}");
    let _ = o.flush();
}

fn signal(name: &str, args: Value) {
    out(json!({ "signal": name, "args": args }));
}

fn reply(id: &Value, r: Result<Value, String>) {
    match r {
        Ok(v) => out(json!({ "id": id, "result": v })),
        Err(e) => out(json!({ "id": id, "error": e })),
    }
}

fn set_state(h: &H, s: &'static str) {
    if h.borrow().state != s {
        h.borrow_mut().state = s;
        signal("StateChanged", json!([s]));
    }
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// linking as a secondary device, a new provisioning address each time
/// the server closes the last one unscanned
async fn link(h: H) {
    loop {
        let store = h.borrow().store.clone();
        let (tx, rx) = oneshot::channel();
        let h2 = h.clone();
        let (r, shown) = future::join(
            Manager::link_secondary_device(store, SignalServers::Production, DEVICE_NAME.into(), tx),
            async move {
                let Ok(url) = rx.await else { return false };
                let url = url.to_string();
                let waiting = {
                    let mut b = h2.borrow_mut();
                    b.qr = Some(url.clone());
                    std::mem::take(&mut b.qr_waiting)
                };
                signal("QRCode", json!([url]));
                for id in waiting {
                    reply(&id, Ok(json!([url])));
                }
                true
            },
        )
        .await;
        h.borrow_mut().qr = None;
        match r {
            Ok(m) => {
                h.borrow_mut().linking = false;
                set_state(&h, "connecting");
                spawn_local(receive(h.clone(), m));
                return;
            }
            Err(e) if shown => {
                // the server closes an unscanned provisioning socket
                // after about 90 s ("Timeout"): the next address
                eprintln!("signal-host: linking: {e}; a new address");
                tokio::time::sleep(Duration::from_secs(1)).await;
            }
            Err(e) => {
                // no address at all: the network or the server; the user
                // interface asks again
                let e = format!("linking: {e}");
                signal("Error", json!([e]));
                let waiting = std::mem::take(&mut h.borrow_mut().qr_waiting);
                for id in waiting {
                    reply(&id, Err(e.clone()));
                }
                h.borrow_mut().linking = false;
                return;
            }
        }
    }
}

fn chat_of_group(d: &DataMessage) -> Option<String> {
    let key = d.group_v2.as_ref()?.master_key.as_ref()?;
    Some(format!("group:{}", key.iter().map(|b| format!("{b:02x}")).collect::<String>()))
}

/// a message as a signal (Message, or History for the store's); receipts
/// and typing as Receipt and ChatState (live ones only)
fn message(c: &Content, name: &str) {
    let sender = c.metadata.sender.raw_uuid().to_string();
    let (chat, from_me, d) = match &c.body {
        ContentBody::DataMessage(d) => (chat_of_group(d).unwrap_or_else(|| sender.clone()), false, d),
        ContentBody::SynchronizeMessage(SyncMessage {
            content:
                Some(SyncContent::Sent(Sent {
                    message: Some(d),
                    destination_service_id,
                    ..
                })),
            ..
        }) => (
            chat_of_group(d).unwrap_or_else(|| destination_service_id.clone().unwrap_or_default()),
            true,
            d,
        ),
        ContentBody::ReceiptMessage(ReceiptMessage { r#type, timestamp }) if name == "Message" => {
            let kind = match r#type.and_then(|t| receipt_message::Type::try_from(t).ok()) {
                Some(receipt_message::Type::Delivery) => "delivered",
                Some(receipt_message::Type::Read) | Some(receipt_message::Type::Viewed) => "read",
                None => return,
            };
            let ids: Vec<String> = timestamp.iter().map(|t| t.to_string()).collect();
            signal("Receipt", json!([sender, sender, ids, kind, now_ms() / 1000]));
            return;
        }
        // a person's typing (a group's carries the group's id, not its key)
        ContentBody::TypingMessage(TypingMessage { action, group_id: None, .. }) if name == "Message" => {
            let started = action.and_then(|a| typing_message::Action::try_from(a).ok())
                == Some(typing_message::Action::Started);
            signal("ChatState", json!([sender, sender, started]));
            return;
        }
        _ => return,
    };
    let Some(text) = &d.body else { return };
    let ms = c.metadata.client_timestamp.timestamp_millis();
    signal(
        name,
        json!([ms.to_string(), chat, sender, "", ms / 1000, from_me, "text", text, "", 0]),
    );
}

/// the store's newest messages of every contact and group, as History
async fn history(store: &SqliteStore) {
    let mut threads = Vec::new();
    if let Ok(contacts) = store.contacts().await {
        threads.extend(contacts.flatten().map(|c| Thread::Contact(ServiceId::Aci(c.uuid.into()))));
    }
    if let Ok(groups) = store.groups().await {
        threads.extend(groups.flatten().map(|(key, _)| Thread::Group(key)));
    }
    for t in threads {
        let Ok(msgs) = store.messages(&t, ..).await else { continue };
        let msgs: Vec<Content> = msgs.flatten().collect();
        for c in msgs.iter().skip(msgs.len().saturating_sub(40)) {
            message(c, "History");
        }
    }
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|b| format!("{b:02x}")).collect()
}

/// [[uuid, name, ""]...] or [["group:<key>", title]...] from the store
async fn names(store: &SqliteStore, groups: bool) -> Result<Value, String> {
    let mut out = Vec::new();
    if groups {
        for (key, g) in store.groups().await.map_err(|e| e.to_string())?.flatten() {
            out.push(json!([format!("group:{}", hex(&key)), g.title]));
        }
    } else {
        for c in store.contacts().await.map_err(|e| e.to_string())?.flatten() {
            out.push(json!([c.uuid.to_string(), c.name, ""]));
        }
    }
    Ok(json!([out]))
}

/// the messages, as long as the connection holds; then again
async fn receive(h: H, mut m: Manager<SqliteStore, Registered>) {
    h.borrow_mut().manager = Some(m.clone());
    loop {
        match m.receive_messages().await {
            Ok(stream) => {
                pin_mut!(stream);
                while let Some(r) = stream.next().await {
                    match r {
                        Received::QueueEmpty => set_state(&h, "connected"),
                        Received::Contacts => {}
                        Received::DecryptionError(id) => signal(
                            "Error",
                            json!([format!("a message from {} not decrypted", id.raw_uuid())]),
                        ),
                        Received::Content(c) => message(&c, "Message"),
                    }
                }
            }
            Err(e) => signal("Error", json!([format!("receiving: {e}")])),
        }
        set_state(&h, "connecting");
        tokio::time::sleep(Duration::from_secs(10)).await;
    }
}

async fn send_text(h: H, chat: String, text: String) -> Result<Value, String> {
    let Some(mut m) = h.borrow().manager.clone() else {
        return Err("not linked".into());
    };
    let ts = now_ms();
    let body = DataMessage {
        body: Some(text),
        timestamp: Some(ts),
        ..Default::default()
    };
    if let Some(hex) = chat.strip_prefix("group:") {
        let mut key = [0u8; 32];
        if hex.len() != 64 {
            return Err("a group's master key is 64 hex digits".into());
        }
        for (i, k) in key.iter_mut().enumerate() {
            *k = u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).map_err(|e| e.to_string())?;
        }
        m.send_message_to_group(&key, body, ts).await.map_err(|e| e.to_string())?;
    } else {
        let uuid = Uuid::parse_str(&chat).map_err(|e| e.to_string())?;
        m.send_message(ServiceId::Aci(uuid.into()), body, ts)
            .await
            .map_err(|e| e.to_string())?;
    }
    Ok(json!([ts.to_string()]))
}

fn string_arg(args: &[Value], i: usize) -> Option<String> {
    args.get(i)?.as_str().map(String::from)
}

fn call(h: &H, line: &str) {
    let v: Value = match serde_json::from_str(line) {
        Ok(v) => v,
        Err(e) => return out(json!({ "error": format!("not JSON: {e}") })),
    };
    let id = v.get("id").cloned().unwrap_or(Value::Null);
    let args = v.get("args").and_then(|a| a.as_array()).cloned().unwrap_or_default();
    match v.get("call").and_then(|c| c.as_str()).unwrap_or("") {
        "Status" => reply(&id, Ok(json!([h.borrow().state]))),
        "GetQR" => {
            let mut b = h.borrow_mut();
            if b.state != "logged-out" {
                drop(b);
                return reply(&id, Err("already linked".into()));
            }
            if let Some(url) = &b.qr {
                let url = url.clone();
                drop(b);
                return reply(&id, Ok(json!([url])));
            }
            b.qr_waiting.push(id);
            if !b.linking {
                b.linking = true;
                drop(b);
                spawn_local(link(h.clone()));
            }
        }
        "SendText" => {
            // (id, chat, text) as Yappari's, or (chat, text)
            let skip = if args.len() == 3 { 1 } else { 0 };
            let (Some(chat), Some(text)) = (string_arg(&args, skip), string_arg(&args, skip + 1)) else {
                return reply(&id, Err("SendText takes 3 arguments: id, chat, text".into()));
            };
            let h = h.clone();
            spawn_local(async move {
                let r = send_text(h, chat.clone(), text).await;
                let sent = r.as_ref().ok().and_then(|v| v[0].as_str().map(String::from));
                reply(&id, r);
                if let Some(ts) = sent {
                    let secs = ts.parse::<u64>().unwrap_or(0) / 1000;
                    signal("Receipt", json!([chat, "", [ts], "server", secs]));
                }
            });
        }
        "GetContacts" | "GetGroups" => {
            let store = h.borrow().store.clone();
            let groups = v.get("call").and_then(|c| c.as_str()) == Some("GetGroups");
            spawn_local(async move {
                reply(&id, names(&store, groups).await);
            });
        }
        "MarkRead" => reply(&id, Ok(json!([]))),
        other => reply(&id, Err(format!("no method \"{other}\""))),
    }
}

fn store_path() -> String {
    let base = std::env::var("XDG_DATA_HOME").ok().filter(|s| !s.is_empty()).unwrap_or_else(|| {
        format!("{}/.local/share", std::env::var("HOME").unwrap_or_else(|_| "/tmp".into()))
    });
    format!("{base}/signal-host")
}

async fn run() -> Result<(), String> {
    let dir = store_path();
    std::fs::create_dir_all(&dir).map_err(|e| format!("{dir}: {e}"))?;
    let store = SqliteStore::open(&format!("sqlite://{dir}/store.db"), OnNewIdentity::Trust)
        .await
        .map_err(|e| format!("the store: {e}"))?;
    let h: H = Rc::new(RefCell::new(Host {
        state: "logged-out",
        store: store.clone(),
        linking: false,
        qr: None,
        qr_waiting: Vec::new(),
        manager: None,
    }));
    if let Ok(m) = Manager::load_registered(store.clone()).await {
        h.borrow_mut().state = "connecting";
        history(&store).await;
        spawn_local(receive(h.clone(), m));
    }
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    while let Ok(Some(line)) = lines.next_line().await {
        if !line.trim().is_empty() {
            call(&h, &line);
        }
    }
    store.close().await;
    Ok(())
}

fn main() {
    if std::env::args().nth(1).as_deref() != Some("-stdio") {
        eprintln!("usage: signal-host -stdio (JSON lines, docs/messengers.md of kolibrios-arm)");
        std::process::exit(2);
    }
    tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "warn".into()),
        )
        .init();
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("the runtime");
    let local = tokio::task::LocalSet::new();
    if let Err(e) = local.block_on(&rt, run()) {
        signal("Error", json!([e]));
        std::process::exit(1);
    }
}
