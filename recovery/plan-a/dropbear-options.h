/* SPDX-License-Identifier: MIT
 * What the developer shell's SSH server may do (tools/build.py dropbear;
 * recovery/plan-a/dev-access). Keys only, of one type; nothing forwarded;
 * no file-transfer subsystem (use "scp -O"). */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_RSA 0
#define DROPBEAR_DSS 0
#define DROPBEAR_ECDSA 0
#define DROPBEAR_ED25519 1
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SFTPSERVER 0
#define INETD_MODE 0
#define DO_MOTD 0
/* Our own tools first, then the stock system's. */
#define DEFAULT_ROOT_PATH "/opt/autotop/sys:/opt/autotop/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#define DEFAULT_PATH DEFAULT_ROOT_PATH
