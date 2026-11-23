#ifndef CHATROOM_H
#define CHATROOM_H

#include "data/config.h"

BOOL RunChatRoom(const Config* config, const char* host);

BOOL ListChatRooms(const Config* config);

#endif
