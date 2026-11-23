# Change Log
All notable changes to this project will be documented in this file as of mailbox v1.0

## mailbox 1.2.0

### Added
- Mailbox now supports chatting with the chat command, make sure to add CHATPATH and CHATSIZE to config!

## mailbox 1.1.0

### Added

- history command to show previous sent files
- Mailboxes can now act as proxies, if you enable PROXY=true in the config, you can add a proxy file which takes in host names and translates them to IPs of known proxies. Files will then be sent to a proxy folder on the proxy machine, which will then be transferred again to the next destination. If a proxy passover fails, it will try again at intervals PINTERVAL set in the config until it continues, or the daemon is closed.
