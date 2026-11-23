# mailbox

Send files between machines. Each machine runs a tiny background receiver that drops incoming files into its mailbox directory, and keeps track of what's new.

## Commands

```
mailbox help                         lists all commands and usages
mailbox version | -v                 outputs the version
mailbox up                           starts the background process that receives mail
mailbox down                         shuts down the background process
mailbox inbox [all]                  lists unread mail and who sent it ("all" includes read mail)
mailbox send <ip|id>[:port] <file>   sends a file to an IP address or a known ID
mailbox clear <all|id...>            marks all mail, or the given mail IDs, as read
mailbox history [all|count]          lists files you've sent (last 20 by default)
mailbox chat [id]                    opens a chat with a mailbox ID, or lists your chats
```

## Config

A default one is generated on first run!

```
PORT: 7570          port to receive on and send to (1024-65535)
ID: my-machine      name this machine introduces itself as
INBOXSIZE: 1G       max total size of the mailbox directory (bytes, or K/M/G/T)
DIRECTORY: inbox    the mailbox directory
DNS: dns            ID -> IP lookup file (auto-generated)
STATE: state        record of received mail + read/unread
HISTORY: history    record of all history
PROXY: false        enables or disables proxy functionality
PROXYPATH: proxies  where proxy entries are stored
PROXYDIR: proxy     where proxy files are stored
PROXYDIRSIZE: 1G    how much room to store proxy files
PINTERVAL: 60       how often proxy forwards are retried
CHATPATH: chat      where chats are stored
CHATSIZE: 10M       how much room to store chats
PASSWORD:           optional; leave empty for none
```

The background receiver reads the config once at startup, so restart it (`mailbox down && mailbox up`) after editing.
