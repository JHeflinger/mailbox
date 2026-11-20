# mailbox

Send files between machines. Each machine runs a tiny background receiver that drops incoming files into its mailbox directory, and keeps track of what's new.

## Commands

```
mailbox help                         lists all commands and usages
mailbox version | -v                 outputs the version
mailbox up                           starts the background receiver
mailbox down                         shuts down the background receiver
mailbox inbox [all]                  lists unread mail ("all" includes read mail)
mailbox send <ip|id>[:port] <file>   sends a file to an IP or a known ID
mailbox clear <all|id...>            marks all mail, or specific mail IDs, as read
```

## Config

Lives at `~/.mailbox/config` (`%USERPROFILE%\.mailbox\config` on Windows), or wherever `MAILBOX_CONFIG` points. A default one is generated on first run. Relative paths resolve against the config's folder, and `~` is expanded.

```
PORT: 7570          port to receive on and send to (1024-65535)
ID: my-machine      name this machine introduces itself as
INBOXSIZE: 1G       max total size of the mailbox directory (bytes, or K/M/G/T)
DIRECTORY: inbox    the mailbox directory
DNS: dns            ID -> IP lookup file (auto-generated)
STATE: state        record of received mail + read/unread
PASSWORD:           optional; leave empty for none
```

The background receiver reads the config once at startup, so restart it (`mailbox down && mailbox up`) after editing.
