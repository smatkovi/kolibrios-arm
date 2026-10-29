/* KolibriOS for ARM: APR, the ADSP's packet router (Linux:
 * drivers/soc/qcom/apr.c), and the voice path of a call on it: the audio
 * front end's codec DMA ports (q6afe) and the voice services (q6mvm,
 * q6cvp) as Linux's q6voice driver starts them on the Pro1X.
 *
 * Plain C like the rest of kernel/modem: the packets go out through a
 * callback (the ADSP's GLINK channel "apr_audio_svc" on the device, a
 * fake ADSP in tests/modem), the replies come in through voicepath_rx. */
#ifndef APR_H
#define APR_H
#include "modem.h"

#define APR_HDR_SIZE 20
#define APR_DOMAIN_ADSP 4
#define APR_DOMAIN_APPS 5
#define APR_SVC_AFE 0x04
#define APR_SVC_MVM 0x09
#define APR_SVC_CVP 0x0b
#define APR_MSG_TYPE_EVENT 0
#define APR_MSG_TYPE_CMD_RSP 1
#define APR_MSG_TYPE_SEQ_CMD 2

#define APR_BASIC_RSP_RESULT 0x000110E8
#define APRV2_IBASIC_CMD_DESTROY_SESSION 0x0001003C

/* the audio front end (q6afe.c) */
#define AFE_PORT_CMD_DEVICE_START 0x000100E5
#define AFE_PORT_CMD_DEVICE_STOP 0x000100E6
#define AFE_PORT_CMD_SET_PARAM_V2 0x000100EF
#define AFE_SVC_CMD_SET_PARAM 0x000100F3
#define AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST 0x000100F4
#define AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST 0x000100F5
#define AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST 0x000100F6
#define AFE_MODULE_AUDIO_DEV_INTERFACE 0x0001020C
#define AFE_PARAM_ID_CODEC_DMA_CONFIG 0x000102B8
#define AFE_MODULE_CLOCK_SET 0x0001028F
#define AFE_PARAM_ID_CLOCK_SET 0x00010290
#define AFE_CLK_TOKEN 1024
#define AFE_PORT_CONFIG_SIZE 36        /* sizeof(union afe_port_config), packed */

#define AFE_PORT_ID_VA_CODEC_DMA_TX_0 0xB021
#define AFE_PORT_ID_RX_CODEC_DMA_RX_0 0xB030

/* the clocks the RX and VA macros take from the DSP (their device tree
 * nodes: q6afecc 57-60 and 103), as Q6 ids */
#define Q6AFE_LPASS_CLK_ID_TX_CORE_MCLK 0x30c
#define Q6AFE_LPASS_CLK_ID_TX_CORE_NPL_MCLK 0x30d
#define Q6AFE_LPASS_CLK_ID_RX_CORE_MCLK 0x30e
#define Q6AFE_LPASS_CLK_ID_RX_CORE_NPL_MCLK 0x30f
#define Q6AFE_LPASS_CORE_HW_MACRO_BLOCK 3
#define Q6AFE_LPASS_CORE_HW_DCODEC_BLOCK 4
#define LPASS_MCLK_HZ 19200000

/* the voice services (q6mvm.c, q6cvp.c) */
#define VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION 0x000110FF
#define VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL 0x00011327
#define VSS_IMVM_CMD_ATTACH_VOCPROC 0x0001123E
#define VSS_IMVM_CMD_DETACH_VOCPROC 0x0001123F
#define VSS_IMVM_CMD_START_VOICE 0x00011190
#define VSS_IMVM_CMD_STOP_VOICE 0x00011192
#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2 0x000112BF
#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3 0x00013169
#define VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT 0x00013198
#define VSS_IVOCPROC_CMD_ENABLE 0x000100C6
#define VSS_IVOCPROC_CMD_DISABLE 0x000110E1
#define VSS_ICOMMON_CMD_SET_PARAM_V2 0x0001133D
#define VSS_PARAM_VOCPROC_TX_CHANNEL_INFO 0x0001328E
#define VSS_PARAM_VOCPROC_RX_CHANNEL_INFO 0x0001328F
#define VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO 0x00013253
#define VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO 0x00013254
#define VSS_IVOCPROC_DIRECTION_RX_TX 2
#define VSS_IVOCPROC_PORT_ID_NONE 0xFFFF
#define VSS_IVOCPROC_TOPOLOGY_ID_NONE 0x00010F70
#define VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT 0x00010F77
#define VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING 0x00010F7C
#define VSS_ICOMMON_CAL_NETWORK_ID_NONE 0x0001135E
/* q6voice's path for the Pro1X's DAI link (VoiceMMode1) and its
 * session name (the modem's VSID) */
#define VOICE_PATH_VOICEMMODE1 6
#define VOICE_SESSION_VOICEMMODE1 "11C05000"

struct apr_hdr {
	u16 hdr_field, pkt_size;
	u8 src_svc, src_domain;
	u16 src_port;
	u8 dest_svc, dest_domain;
	u16 dest_port;
	u32 token, opcode;
};

/* a packet's header, 20 bytes: a command of the apps processor to svc */
void apr_put_hdr(u8 *p, u32 pkt_size, u8 svc, u16 src_port, u16 dest_port, u32 token, u32 opcode);
bool apr_get_hdr(const u8 *p, u32 len, struct apr_hdr *h);

/* The commands, written to p (room for 128 bytes); their length */
u32 afe_hw_vote(u8 *p, u32 block, const char *client);
u32 afe_hw_devote(u8 *p, u32 block, u32 handle);
u32 afe_clock_set(u8 *p, u32 clk_id, u32 hz);
u32 afe_cdc_dma_config(u8 *p, u16 port, u16 token, u32 rate, u16 bits, u16 channels);
u32 afe_port_start(u8 *p, u16 port, u16 token);
u32 afe_port_stop(u8 *p, u16 port, u16 token);
u32 mvm_create_passive(u8 *p, u16 path, const char *name);
u32 mvm_dual_control(u8 *p, u16 path, u16 handle);
u32 mvm_attach(u8 *p, u16 path, u16 handle, u16 cvp_handle, bool attach);
u32 mvm_start(u8 *p, u16 path, u16 handle, bool start);
u32 cvp_create(u8 *p, u16 path, bool v3, u16 tx_port, u16 rx_port);
u32 cvp_channel_info(u8 *p, u16 path, u16 handle, bool tx);
u32 cvp_media_format(u8 *p, u16 path, u16 handle, u16 port, bool tx);
u32 cvp_topology_commit(u8 *p, u16 path, u16 handle);
u32 cvp_enable(u8 *p, u16 path, u16 handle, bool enable);
u32 voice_destroy(u8 *p, u8 svc, u16 path, u16 handle);

/* The voice path of a call, a sequence of commands one at a time, each
 * waiting for its reply (300 ms, as Linux's q6voice) */
enum { VP_IDLE, VP_STARTING, VP_RUNNING, VP_STOPPING, VP_FAILED };

struct voicepath {
	int (*send)(void *arg, const u8 *pkt, u32 len);
	void *arg;
	bool cvd_v2_3;                 /* the DSP's CVD 2.3 (create v3 and its parameters) */
	bool ports;                    /* the clocks and codec DMA ports too (false: running already) */
	int state;
	bool unwinding;                /* undoing a start that failed: VP_FAILED at the end */
	bool down_wanted;              /* stopped while starting: down after the step under way */
	bool chan_err;                 /* the channel took no command */
	int seq, step;                 /* the sequence running and its step */
	u32 expect_op, sent_ms;
	u8 expect_svc;
	u16 mvm, cvp;                  /* the sessions' handles */
	u32 vote_macro, vote_dcodec;   /* the AFE's client handles for the hardware votes */
	u32 status;                    /* the DSP's last error */
	u32 cmds, replies;
	char err[64];
};

/* ports: the votes, clocks and codec DMA ports as well; false when they
 * run already (Linux's streams left them on, docs/pro1x-audio.md), as
 * Linux's q6voice starts only MVM and CVP */
void voicepath_init(struct voicepath *v, int (*send)(void *, const u8 *, u32), void *arg, bool cvd_v2_3,
		    bool ports);
/* the call is up: clocks, codec DMA ports, MVM and CVP (0: begun) */
int voicepath_up(struct voicepath *v, u32 ms);
/* the call is over: all of it the other way round (while starting: once
 * the command under way is answered) */
int voicepath_down(struct voicepath *v, u32 ms);
void voicepath_rx(struct voicepath *v, const u8 *pkt, u32 len, u32 ms);
void voicepath_poll(struct voicepath *v, u32 ms);

#endif
