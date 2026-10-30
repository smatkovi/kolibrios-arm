// fakepeer: a messenger backend for tests (docs/messengers.md). It speaks
// the JSON lines of yappari-wa -stdio, purple-host -stdio and signal-host
// -stdio from fixtures, without a network or an account:
//
//	fakepeer -stdio [-service whatsapp|telegram|signal] [-every SECONDS]
//
// The service comes from the program's name when it is installed as
// /bin/yappari-wa, /bin/purple-host or /bin/signal-host. It is linked
// ("connected") from the start: Status, GetContacts and GetGroups answer
// from the fixtures, History signals give each chat a few messages after
// the first Status, a Message from the third contact comes every SECONDS
// (10), SendText answers with an id and Receipts (server, delivered, read)
// and a reply two seconds later. What it is asked goes to standard error
// ("fakepeer: SendText ..."), on KolibriOS the kernel's console.
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

type call struct {
	ID   json.RawMessage   `json:"id"`
	Call string            `json:"call"`
	Args []json.RawMessage `json:"args"`
}

var (
	mu  sync.Mutex
	out = bufio.NewWriter(os.Stdout)
	seq int
)

func send(v map[string]interface{}) {
	line, _ := json.Marshal(v)
	mu.Lock()
	out.Write(line)
	out.WriteByte('\n')
	out.Flush()
	mu.Unlock()
}

func signal(name string, args ...interface{}) {
	send(map[string]interface{}{"signal": name, "args": args})
}

func reply(id json.RawMessage, result ...interface{}) {
	if result == nil {
		result = []interface{}{}
	}
	send(map[string]interface{}{"id": id, "result": result})
}

func fail(id json.RawMessage, e string) {
	send(map[string]interface{}{"id": id, "error": e})
}

type contact struct{ ID, Name, Push string }
type group struct{ ID, Subject string }
type hist struct {
	Chat, From string
	Ago        time.Duration
	Out        bool
	Text       string
}

type fixture struct {
	account  string
	contacts []contact
	groups   []group
	history  []hist
	live     string // the chat (and sender) of the live messages
}

func newID(service string) string {
	mu.Lock()
	seq++
	n := seq
	mu.Unlock()
	switch service {
	case "whatsapp":
		return fmt.Sprintf("3EB0FA4E%012X", n)
	case "telegram":
		return fmt.Sprintf("tg-%d", n)
	}
	return fmt.Sprintf("%d", time.Now().UnixMilli()+int64(n))
}

func fixtures(service string) fixture {
	var c [3]string
	var g string
	f := fixture{}
	switch service {
	case "whatsapp":
		c = [3]string{"4366411111111@s.whatsapp.net", "4366422222222@s.whatsapp.net", "4366433333333@s.whatsapp.net"}
		g = "120363000000000001@g.us"
		f.account = "4366400000000@s.whatsapp.net"
	case "telegram":
		c = [3]string{"id1001", "id1002", "id1003"}
		g = "chat-1001234567890"
		f.account = "+99912345678"
	default:
		c = [3]string{"8a5e3c1e-0000-4000-8000-000000000001", "8a5e3c1e-0000-4000-8000-000000000002",
			"8a5e3c1e-0000-4000-8000-000000000003"}
		g = "group:" + strings.Repeat("0f", 32)
		f.account = "8a5e3c1e-0000-4000-8000-000000000000"
	}
	f.contacts = []contact{{c[0], "Anna Berger", "Anna"}, {c[1], "Bernhard Koller", "Berni"}, {c[2], "", "Clara"}}
	f.groups = []group{{g, "Chor Probe"}}
	f.history = []hist{
		{c[0], c[0], 26 * time.Hour, false, "Hallo! Kommst du morgen zur Probe?"},
		{c[0], "", 25 * time.Hour, true, "Ja, um 19 Uhr."},
		{c[0], c[0], 3 * time.Hour, false, "Super, bis dann <3"},
		{c[1], c[1], 2 * time.Hour, false, "Grüße aus Graz"},
		{g, c[0], 90 * time.Minute, false, "Probe morgen 19:00"},
		{g, c[1], 80 * time.Minute, false, "Ich bringe die Noten mit"},
	}
	f.live = c[2]
	return f
}

// a message in the protocol's shape: id, chat, sender, pushname, time,
// fromMe, kind, text, mime type, size
func message(name, service string, f fixture, chat, from string, t time.Time, outgoing bool, text string) {
	push := ""
	for _, ct := range f.contacts {
		if ct.ID == from {
			push = ct.Push
		}
	}
	if outgoing {
		from = f.account
	}
	signal(name, newID(service), chat, from, push, t.Unix(), outgoing, "text", text, "", 0)
}

func str(raw json.RawMessage) string {
	var s string
	json.Unmarshal(raw, &s)
	return s
}

func main() {
	stdio := flag.Bool("stdio", false, "the JSON lines on standard input and output")
	service := flag.String("service", "", "whatsapp, telegram or signal (else from the program's name)")
	every := flag.Int("every", 10, "seconds between the live messages (0: none)")
	flag.Parse()
	if !*stdio {
		fmt.Fprintln(os.Stderr, "usage: fakepeer -stdio [-service whatsapp|telegram|signal] [-every SECONDS]")
		os.Exit(2)
	}
	if *service == "" {
		switch filepath.Base(os.Args[0]) {
		case "purple-host":
			*service = "telegram"
		case "signal-host":
			*service = "signal"
		default:
			*service = "whatsapp"
		}
	}
	f := fixtures(*service)
	fmt.Fprintf(os.Stderr, "fakepeer: %s, %d contacts, %d groups\n", *service, len(f.contacts), len(f.groups))
	var once sync.Once
	start := func() {
		now := time.Now()
		for _, h := range f.history {
			message("History", *service, f, h.Chat, h.From, now.Add(-h.Ago), h.Out, h.Text)
		}
		if *every > 0 {
			go func() {
				for n := 1; ; n++ {
					time.Sleep(time.Duration(*every) * time.Second)
					message("Message", *service, f, f.live, f.live, time.Now(), false, fmt.Sprintf("Live message %d", n))
				}
			}()
		}
	}
	in := bufio.NewScanner(os.Stdin)
	in.Buffer(make([]byte, 64<<10), 1<<20)
	for in.Scan() {
		var c call
		if err := json.Unmarshal(in.Bytes(), &c); err != nil {
			fail(nil, "not a call: "+err.Error())
			continue
		}
		switch c.Call {
		case "Status":
			if *service == "telegram" {
				reply(c.ID, "connected", f.account)
			} else {
				reply(c.ID, "connected", f.account)
			}
			once.Do(start)
		case "GetContacts":
			list := []interface{}{}
			for _, ct := range f.contacts {
				list = append(list, []string{ct.ID, ct.Name, ct.Push})
			}
			reply(c.ID, list)
		case "GetGroups":
			list := []interface{}{}
			for _, g := range f.groups {
				list = append(list, []string{g.ID, g.Subject})
			}
			reply(c.ID, list)
		case "SendText":
			if len(c.Args) != 3 {
				fail(c.ID, "SendText takes 3 arguments")
				continue
			}
			id, chat, text := str(c.Args[0]), str(c.Args[1]), str(c.Args[2])
			if id == "" {
				id = newID(*service)
			}
			fmt.Fprintf(os.Stderr, "fakepeer: SendText %s %q\n", chat, text)
			reply(c.ID, id)
			go func() {
				time.Sleep(300 * time.Millisecond)
				signal("Receipt", chat, "", []string{id}, "server", time.Now().Unix())
				time.Sleep(700 * time.Millisecond)
				signal("Receipt", chat, chat, []string{id}, "delivered", time.Now().Unix())
				time.Sleep(time.Second)
				signal("Receipt", chat, chat, []string{id}, "read", time.Now().Unix())
				from := chat
				if strings.HasSuffix(chat, "@g.us") || strings.HasPrefix(chat, "chat") || strings.HasPrefix(chat, "group:") {
					from = f.contacts[0].ID
				}
				message("Message", *service, f, chat, from, time.Now(), false, "Re: "+text)
			}()
		case "MarkRead":
			fmt.Fprintf(os.Stderr, "fakepeer: MarkRead %s\n", in.Text())
			reply(c.ID)
		case "GetQR":
			fail(c.ID, "already linked")
		default:
			fail(c.ID, fmt.Sprintf("no method %q", c.Call))
		}
	}
}
