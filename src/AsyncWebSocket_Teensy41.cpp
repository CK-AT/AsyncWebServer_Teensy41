/****************************************************************************************************************************
  AsyncWebSocket_Teensy41.cpp - Dead simple AsyncWebServer for Teensy41 QNEthernet

  For Teensy41 with QNEthernet

  AsyncWebServer_Teensy41 is a library for the Teensy41 with QNEthernet

  Based on and modified from ESPAsyncWebServer (https://github.com/me-no-dev/ESPAsyncWebServer)
  Built by Khoi Hoang https://github.com/khoih-prog/AsyncWebServer_Teensy41

  Copyright (c) 2016 Hristo Gochkov. All rights reserved.
  This file is part of the esp8266 core for Arduino environment.
  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
  as published bythe Free Software Foundation, either version 3 of the License, or (at your option) any later version.
  This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
  You should have received a copy of the GNU General Public License along with this program.
  If not, see <https://www.gnu.org/licenses/>.

  Version: 1.7.0

  Version Modified By   Date      Comments
  ------- -----------  ---------- -----------
  1.4.1   K Hoang      18/03/2022 Initial coding for Teensy 4.1 using built-in QNEthernet.
                                  Bump up version to v1.4.1 to sync with AsyncWebServer_STM32 v1.4.1
  1.5.0   K Hoang      01/10/2022 Fix issue with slow browsers or network. Add function and example to support favicon.ico
  1.6.0   K Hoang      06/10/2022 Option to use non-destroyed cString instead of String to save Heap
  1.6.1   K Hoang      10/11/2022 Add examples to demo how to use beginChunkedResponse() to send in chunks
  1.6.2   K Hoang      16/01/2023 Add examples Async_WebSocketsServer
  1.7.0   K Hoang      29/01/2024 Fix file upload handler by adjusting function signatures
 *****************************************************************************************************************************/

#include "Arduino.h"

#if !defined(_AWS_TEENSY41_LOGLEVEL_)
  #define _AWS_TEENSY41_LOGLEVEL_     1
#endif

#include "AsyncWebServer_Teensy41_Debug.h"

#include "AsyncWebSocket_Teensy41.h"

#include <libb64/cencode.h>

// For Teensy41
#include "Crypto/sha1.h"
#include "Crypto/Hash.h"

/////////////////////////////////////////////////

#define MAX_PRINTF_LEN 64

/////////////////////////////////////////////////

char *ltrim(char *s)
{
  while (isspace(*s))
    s++;

  return s;
}

/////////////////////////////////////////////////

char *rtrim(char *s)
{
  char* back = s + strlen(s);

  while (isspace(*--back));

  *(back + 1) = '\0';

  return s;
}

/////////////////////////////////////////////////

char *trim(char *s)
{
  return rtrim(ltrim(s));
}

/////////////////////////////////////////////////

size_t b64_encoded_size(size_t inlen)
{
  size_t ret;
  ret = inlen;

  if (inlen % 3 != 0)
    ret += 3 - (inlen % 3);

  ret /= 3;
  ret *= 4;

  return ret;
}

/////////////////////////////////////////////////

char * b64_encode(const unsigned char *in, size_t len, char * out)
{
  const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t  elen;
  size_t  i;
  size_t  j;
  size_t  v;

  if (in == NULL || len == 0)
    return NULL;

  elen = b64_encoded_size(len);
  out[elen] = '\0';

  for (i = 0, j = 0; i < len; i += 3, j += 4)
  {
    v = in[i];
    v = i + 1 < len ? v << 8 | in[i + 1] : v << 8;
    v = i + 2 < len ? v << 8 | in[i + 2] : v << 8;

    out[j]   = b64chars[(v >> 18) & 0x3F];
    out[j + 1] = b64chars[(v >> 12) & 0x3F];

    if (i + 1 < len)
    {
      out[j + 2] = b64chars[(v >> 6) & 0x3F];
    }
    else
    {
      out[j + 2] = '=';
    }

    if (i + 2 < len)
    {
      out[j + 3] = b64chars[v & 0x3F];
    }
    else
    {
      out[j + 3] = '=';
    }
  }

  return out;
}

/////////////////////////////////////////////////

size_t webSocketSendFrameWindow(AsyncClient *client)
{
  if (!client->canSend())
    return 0;

  size_t space = client->space();

  if (space < 9)
    return 0;

  return (space - 8);
}

/////////////////////////////////////////////////

size_t webSocketSendFrame(AsyncClient *client, bool final, uint8_t opcode, bool mask, uint8_t *data, size_t len)
{
  if (!client->canSend())
    return 0;

  size_t space = client->space();

  if (space < 2)
    return 0;

  uint8_t mbuf[4] = {0, 0, 0, 0};
  uint8_t headLen = 2;

  if (len && mask)
  {
    headLen += 4;
    mbuf[0] = rand() % 0xFF;
    mbuf[1] = rand() % 0xFF;
    mbuf[2] = rand() % 0xFF;
    mbuf[3] = rand() % 0xFF;
  }

  if (len > 125)
    headLen += 2;

  if (space < headLen)
    return 0;

  space -= headLen;

  if (len > space)
    len = space;

  uint8_t *buf = (uint8_t*)malloc(headLen);

  if (buf == NULL)
  {
    LOGDEBUG1("Could not malloc for frame header, bytes = ", headLen);

    return 0;
  }

  buf[0] = opcode & 0x0F;

  if (final)
    buf[0] |= 0x80;

  if (len < 126)
    buf[1] = len & 0x7F;
  else
  {
    buf[1] = 126;
    buf[2] = (uint8_t)((len >> 8) & 0xFF);
    buf[3] = (uint8_t)(len & 0xFF);
  }

  if (len && mask)
  {
    buf[1] |= 0x80;
    memcpy(buf + (headLen - 4), mbuf, 4);
  }

  if (client->add((const char *)buf, headLen) != headLen)
  {
    LOGDEBUG1("Error adding header, bytes =", headLen);

    free(buf);
    return 0;
  }

  free(buf);

  if (len)
  {
    if (len && mask)
    {
      size_t i;

      for (i = 0; i < len; i++)
        data[i] = data[i] ^ mbuf[i % 4];
    }

    if (client->add((const char *)data, len) != len)
    {
      LOGDEBUG1("Error adding data, bytes =", len);

      return 0;
    }
  }

  if (!client->send())
  {
    LOGDEBUG1("Error sending frame: bytes =", headLen + len);
    return 0;
  }

  return len;
}

/////////////////////////////////////////////////
/////////////////////////////////////////////////

/*
      AsyncWebSocketMessageBuffer
*/

/////////////////////////////////////////////////

AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer()
  : _data(nullptr), _len(0), _lock(false), _count(0)
{
}

/////////////////////////////////////////////////

AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer(uint8_t * data, size_t size)
  : _data(nullptr), _len(size), _lock(false), _count(0)
{
  if (!data)
  {
    return;
  }

  _data = new uint8_t[_len + 1];

  if (_data)
  {
    memcpy(_data, data, _len);
    _data[_len] = 0;
  }
}

/////////////////////////////////////////////////

AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer(size_t size)
  : _data(nullptr), _len(size), _lock(false), _count(0)
{
  _data = new uint8_t[_len + 1];

  if (_data)
  {
    _data[_len] = 0;
  }
}

/////////////////////////////////////////////////

AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer(const AsyncWebSocketMessageBuffer & copy)
  : _data(nullptr), _len(0), _lock(false), _count(0)
{
  _len = copy._len;
  _lock = copy._lock;
  _count = 0;

  if (_len)
  {
    _data = new uint8_t[_len + 1];
    _data[_len] = 0;
  }

  if (_data)
  {
    memcpy(_data, copy._data, _len);
    _data[_len] = 0;
  }
}

/////////////////////////////////////////////////

AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer(AsyncWebSocketMessageBuffer && copy)
  : _data(nullptr), _len(0), _lock(false), _count(0)
{
  _len = copy._len;
  _lock = copy._lock;
  _count = 0;

  if (copy._data)
  {
    _data = copy._data;
    copy._data = nullptr;
  }
}

/////////////////////////////////////////////////

AsyncWebSocketMessageBuffer::~AsyncWebSocketMessageBuffer()
{
  if (_data)
  {
    delete[] _data;
  }
}

/////////////////////////////////////////////////

bool AsyncWebSocketMessageBuffer::reserve(size_t size)
{
  _len = size;

  if (_data)
  {
    delete[] _data;
    _data = nullptr;
  }

  _data = new uint8_t[_len + 1];

  if (_data)
  {
    _data[_len] = 0;

    return true;
  }
  else
  {
    return false;
  }
}

/////////////////////////////////////////////////
/////////////////////////////////////////////////

/*
   Control Frame
*/

class AsyncWebSocketControl
{
  private:
    uint8_t _opcode;
    uint8_t *_data;
    size_t _len;
    bool _mask;
    bool _finished;

  public:

    /////////////////////////////////////////////////

    AsyncWebSocketControl(uint8_t opcode, uint8_t *data = NULL, size_t len = 0, bool mask = false)
      : _opcode(opcode), _len(len), _mask(len && mask), _finished(false)
    {
      if (data == NULL)
        _len = 0;

      if (_len)
      {
        if (_len > 125)
          _len = 125;

        _data = (uint8_t*) malloc(_len);

        if (_data == NULL)
          _len = 0;
        else
          memcpy(_data, data, len);
      }
      else
        _data = NULL;
    }

    /////////////////////////////////////////////////

    virtual ~AsyncWebSocketControl()
    {
      if (_data != NULL)
        free(_data);
    }

    /////////////////////////////////////////////////

    virtual bool finished() const
    {
      return _finished;
    }

    /////////////////////////////////////////////////

    inline uint8_t opcode()
    {
      return _opcode;
    }

    /////////////////////////////////////////////////

    inline uint8_t len()
    {
      return _len + 2;
    }

    /////////////////////////////////////////////////

    size_t send(AsyncClient *client)
    {
      _finished = true;
      return webSocketSendFrame(client, true, _opcode & 0x0F, _mask, _data, _len);
    }
};

/////////////////////////////////////////////////
/////////////////////////////////////////////////

/*
   Basic Buffered Message
*/
AsyncWebSocketBasicMessage::AsyncWebSocketBasicMessage(const char * data, size_t len, uint8_t opcode, bool mask)
  : _len(len), _sent(0), _ack(0), _acked(0)
{
  _opcode = opcode & 0x07;
  _mask = mask;
  _data = (uint8_t*)malloc(_len + 1);

  if (_data == NULL)
  {
    _len = 0;
    _status = WS_MSG_ERROR;
  }
  else
  {
    _status = WS_MSG_SENDING;
    memcpy(_data, data, _len);
    _data[_len] = 0;
  }
}

/////////////////////////////////////////////////

AsyncWebSocketBasicMessage::AsyncWebSocketBasicMessage(uint8_t opcode, bool mask)
  : _len(0), _sent(0), _ack(0), _acked(0), _data(NULL)
{
  _opcode = opcode & 0x07;
  _mask = mask;
}

/////////////////////////////////////////////////

AsyncWebSocketBasicMessage::~AsyncWebSocketBasicMessage()
{
  if (_data != NULL)
    free(_data);
}

/////////////////////////////////////////////////

void AsyncWebSocketBasicMessage::ack(size_t len, uint32_t time)
{
  AWS_TEENSY41_UNUSED(time);

  _acked += len;

  if (_sent == _len && _acked == _ack)
  {
    _status = WS_MSG_SENT;
  }
}

/////////////////////////////////////////////////

size_t AsyncWebSocketBasicMessage::send(AsyncClient *client)
{
  if (_status != WS_MSG_SENDING)
    return 0;

  if (_acked < _ack)
  {
    return 0;
  }

  if (_sent == _len)
  {
    if (_acked == _ack)
      _status = WS_MSG_SENT;

    return 0;
  }

  if (_sent > _len)
  {
    _status = WS_MSG_ERROR;

    return 0;
  }

  size_t toSend = _len - _sent;
  size_t window = webSocketSendFrameWindow(client);

  if (window < toSend)
  {
    toSend = window;
  }

  _sent += toSend;
  _ack += toSend + ((toSend < 126) ? 2 : 4) + (_mask * 4);

  bool final = (_sent == _len);
  uint8_t* dPtr = (uint8_t*)(_data + (_sent - toSend));
  uint8_t opCode = (toSend && _sent == toSend) ? _opcode : (uint8_t)WS_CONTINUATION;

  size_t sent = webSocketSendFrame(client, final, opCode, _mask, dPtr, toSend);
  _status = WS_MSG_SENDING;

  if (toSend && sent != toSend)
  {
    _sent -= (toSend - sent);
    _ack -= (toSend - sent);
  }

  return sent;
}

/////////////////////////////////////////////////

bool AsyncWebSocketBasicMessage::reserve(size_t size)
{
  if (size)
  {
    _data = (uint8_t*) malloc(size + 1);

    if (_data)
    {
      memset(_data, 0, size);
      _len = size;
      _status = WS_MSG_SENDING;

      return true;
    }
  }

  return false;
}

/////////////////////////////////////////////////
/////////////////////////////////////////////////

/*
   AsyncWebSocketMultiMessage Message
*/
AsyncWebSocketMultiMessage::AsyncWebSocketMultiMessage(AsyncWebSocketMessageBuffer * buffer, uint8_t opcode, bool mask)
  : _len(0), _sent(0), _ack(0), _acked(0), _WSbuffer(nullptr)
{

  _opcode = opcode & 0x07;
  _mask = mask;

  if (buffer)
  {
    _WSbuffer = buffer;
    (*_WSbuffer)++;
    _data = buffer->get();
    _len = buffer->length();
    _status = WS_MSG_SENDING;

    LOGDEBUG1("M:", _len);
  }
  else
  {
    _status = WS_MSG_ERROR;
  }
}

/////////////////////////////////////////////////

AsyncWebSocketMultiMessage::~AsyncWebSocketMultiMessage()
{
  if (_WSbuffer)
  {
    (*_WSbuffer)--; // decreases the counter.
  }
}

/////////////////////////////////////////////////

void AsyncWebSocketMultiMessage::ack(size_t len, uint32_t time)
{
  AWS_TEENSY41_UNUSED(time);

  _acked += len;

  if (_sent >= _len && _acked >= _ack)
  {
    _status = WS_MSG_SENT;
  }

  LOGDEBUG1("ACK:", _len);
}

/////////////////////////////////////////////////

size_t AsyncWebSocketMultiMessage::send(AsyncClient *client)
{
  if (_status != WS_MSG_SENDING)
    return 0;

  if (_acked < _ack)
  {
    return 0;
  }

  if (_sent == _len)
  {
    _status = WS_MSG_SENT;

    return 0;
  }

  if (_sent > _len)
  {
    _status = WS_MSG_ERROR;
    LOGDEBUG2("WS_MSG_ERROR:", _sent, _len);

    return 0;
  }

  size_t toSend = _len - _sent;
  size_t window = webSocketSendFrameWindow(client);

  if (window < toSend)
  {
    toSend = window;
  }

  _sent += toSend;
  _ack += toSend + ((toSend < 126) ? 2 : 4) + (_mask * 4);

  LOGDEBUG2("W:", _sent - toSend, toSend);

  bool final = (_sent == _len);
  uint8_t* dPtr = (uint8_t*)(_data + (_sent - toSend));
  uint8_t opCode = (toSend && _sent == toSend) ? _opcode : (uint8_t)WS_CONTINUATION;

  size_t sent = webSocketSendFrame(client, final, opCode, _mask, dPtr, toSend);
  _status = WS_MSG_SENDING;

  if (toSend && sent != toSend)
  {
    LOGDEBUG3("Error Send: toSend =", toSend, "!= sent =", sent);
    _sent -= (toSend - sent);
    _ack -= (toSend - sent);
  }

  LOGDEBUG3("Send OK: _sent = ", _sent, "= sent =", sent);

  return sent;
}

/////////////////////////////////////////////////
/////////////////////////////////////////////////

/*
   Async WebSocket Client
*/
const char * AWSC_PING_PAYLOAD = "ESPAsyncWebServer-PING";
const size_t AWSC_PING_PAYLOAD_LEN = 22;

/////////////////////////////////////////////////

AsyncWebSocketClient::AsyncWebSocketClient(AsyncWebServerRequest *request, AsyncWebSocket *server)
  : _controlQueue(LinkedList<AsyncWebSocketControl * >([](AsyncWebSocketControl * c)
{
  delete  c;
}))
, _messageQueue(LinkedList<AsyncWebSocketMessage *>([](AsyncWebSocketMessage *m)
{
  delete  m;
}))
, _tempObject(NULL)
{
  _client = request->client();
  _server = server;
  _clientId = _server->_getNextId();
  _status = WS_CONNECTED;
  _pstate = 0;
  _lastMessageTime = millis();
  _keepAlivePeriod = 0;
  _client->setRxTimeout(0);

  _client->onError([](void *r, AsyncClient * c, int8_t error)
  {
    AWS_TEENSY41_UNUSED(c);

    ((AsyncWebSocketClient*)(r))->_onError(error);
  }, this);

  _client->onAck([](void *r, AsyncClient * c, size_t len, uint32_t time)
  {
    AWS_TEENSY41_UNUSED(c);

    ((AsyncWebSocketClient*)(r))->_onAck(len, time);
  }, this);

  _client->onDisconnect([](void *r, AsyncClient * c)
  {
    ((AsyncWebSocketClient*)(r))->_onDisconnect();
    delete c;
  }, this);

  _client->onTimeout([](void *r, AsyncClient * c, uint32_t time)
  {
    AWS_TEENSY41_UNUSED(c);

    ((AsyncWebSocketClient*)(r))->_onTimeout(time);
  }, this);

  _client->onData([](void *r, AsyncClient * c, void *buf, size_t len)
  {
    AWS_TEENSY41_UNUSED(c);

    ((AsyncWebSocketClient*)(r))->_onData(buf, len);
  }, this);

  _client->onPoll([](void *r, AsyncClient * c)
  {
    AWS_TEENSY41_UNUSED(c);

    ((AsyncWebSocketClient*)(r))->_onPoll();
  }, this);

  _server->_addClient(this);
  _server->_handleEvent(this, WS_EVT_CONNECT, request, NULL, 0);

  delete request;
}

/////////////////////////////////////////////////

AsyncWebSocketClient::~AsyncWebSocketClient()
{
  _messageQueue.free();
  _controlQueue.free();
  _server->_handleEvent(this, WS_EVT_DISCONNECT, NULL, NULL, 0);
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::_onAck(size_t len, uint32_t time)
{
  _lastMessageTime = millis();

  if (!_controlQueue.isEmpty())
  {
    auto head = _controlQueue.front();

    if (head->finished())
    {
      len -= head->len();

      if (_status == WS_DISCONNECTING && head->opcode() == WS_DISCONNECT)
      {
        _controlQueue.remove(head);
        _status = WS_DISCONNECTED;
        _client->close(true);

        return;
      }

      _controlQueue.remove(head);
    }
  }

  if (len && !_messageQueue.isEmpty())
  {
    _messageQueue.front()->ack(len, time);
  }

  _server->_cleanBuffers();
  _runQueue();
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::_onPoll()
{
  if (_client->canSend() && (!_controlQueue.isEmpty() || !_messageQueue.isEmpty()))
  {
    _runQueue();
  }
  else if (_keepAlivePeriod > 0 && _controlQueue.isEmpty() && _messageQueue.isEmpty()
           && (millis() - _lastMessageTime) >= _keepAlivePeriod)
  {
    ping((uint8_t *)AWSC_PING_PAYLOAD, AWSC_PING_PAYLOAD_LEN);
  }
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::_runQueue()
{
  while (!_messageQueue.isEmpty() && _messageQueue.front()->finished())
  {
    _messageQueue.remove(_messageQueue.front());
  }

  if (!_controlQueue.isEmpty() && (_messageQueue.isEmpty() || _messageQueue.front()->betweenFrames())
      && webSocketSendFrameWindow(_client) > (size_t)(_controlQueue.front()->len() - 1))
  {
    _controlQueue.front()->send(_client);
  }
  else if (!_messageQueue.isEmpty() && _messageQueue.front()->betweenFrames() && webSocketSendFrameWindow(_client))
  {
    _messageQueue.front()->send(_client);
  }
}

/////////////////////////////////////////////////

bool AsyncWebSocketClient::queueIsFull()
{
  if ((_messageQueue.length() >= WS_MAX_QUEUED_MESSAGES) || (_status != WS_CONNECTED) )
    return true;

  return false;
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::_queueMessage(AsyncWebSocketMessage *dataMessage)
{
  if (dataMessage == NULL)
    return;

  if (_status != WS_CONNECTED)
  {
    delete dataMessage;
    return;
  }

  if (_messageQueue.length() >= WS_MAX_QUEUED_MESSAGES)
  {
    LOGERROR("ERROR: Too many messages queued");
    delete dataMessage;
  }
  else
  {
    _messageQueue.add(dataMessage);
  }

  if (_client->canSend())
    _runQueue();
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::_queueControl(AsyncWebSocketControl *controlMessage)
{
  if (controlMessage == NULL)
    return;

  _controlQueue.add(controlMessage);

  if (_client->canSend())
    _runQueue();
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::close(uint16_t code, const char * message)
{
  if (_status != WS_CONNECTED)
    return;

  if (code)
  {
    uint8_t packetLen = 2;

    if (message != NULL)
    {
      size_t mlen = strlen(message);

      if (mlen > 123)
        mlen = 123;

      packetLen += mlen;
    }

    char * buf = (char*) malloc(packetLen);

    if (buf != NULL)
    {
      buf[0] = (uint8_t)(code >> 8);
      buf[1] = (uint8_t)(code & 0xFF);

      if (message != NULL)
      {
        memcpy(buf + 2, message, packetLen - 2);
      }

      _queueControl(new AsyncWebSocketControl(WS_DISCONNECT, (uint8_t*)buf, packetLen));
      free(buf);

      return;
    }
  }

  _queueControl(new AsyncWebSocketControl(WS_DISCONNECT));
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::ping(uint8_t *data, size_t len)
{
  if (_status == WS_CONNECTED)
    _queueControl(new AsyncWebSocketControl(WS_PING, data, len));
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::_onError(int8_t) {}

/////////////////////////////////////////////////

void AsyncWebSocketClient::_onTimeout(uint32_t time)
{
  AWS_TEENSY41_UNUSED(time);

  _client->close(true);
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::_onDisconnect()
{
  _client = NULL;
  _server->_handleDisconnect(this);
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::_onData(void *pbuf, size_t plen)
{
  _lastMessageTime = millis();
  uint8_t *data = (uint8_t*)pbuf;

  while (plen > 0)
  {
    if (!_pstate)
    {
      const uint8_t *fdata = data;
      _pinfo.index = 0;
      _pinfo.final = (fdata[0] & 0x80) != 0;
      _pinfo.opcode = fdata[0] & 0x0F;
      _pinfo.masked = (fdata[1] & 0x80) != 0;
      _pinfo.len = fdata[1] & 0x7F;
      data += 2;
      plen -= 2;

      if (_pinfo.len == 126)
      {
        _pinfo.len = fdata[3] | (uint16_t)(fdata[2]) << 8;
        data += 2;
        plen -= 2;
      }
      else if (_pinfo.len == 127)
      {
        _pinfo.len = fdata[9] | (uint16_t)(fdata[8]) << 8 | (uint32_t)(fdata[7]) << 16 | (uint32_t)(fdata[6]) << 24
                     | (uint64_t)(fdata[5]) << 32 | (uint64_t)(fdata[4]) << 40 | (uint64_t)(fdata[3]) << 48
                     | (uint64_t)(fdata[2]) << 56;
        data += 8;
        plen -= 8;
      }

      if (_pinfo.masked)
      {
        memcpy(_pinfo.mask, data, 4);
        data += 4;
        plen -= 4;
      }
    }

    const size_t datalen = std::min((size_t)(_pinfo.len - _pinfo.index), plen);
    const auto datalast = data[datalen];

    if (_pinfo.masked)
    {
      for (size_t i = 0; i < datalen; i++)
        data[i] ^= _pinfo.mask[(_pinfo.index + i) % 4];
    }

    if ((datalen + _pinfo.index) < _pinfo.len)
    {
      _pstate = 1;

      if (_pinfo.index == 0)
      {
        if (_pinfo.opcode)
        {
          _pinfo.message_opcode = _pinfo.opcode;
          _pinfo.num = 0;
        }
        else
          _pinfo.num += 1;
      }

      _server->_handleEvent(this, WS_EVT_DATA, (void *)&_pinfo, (uint8_t*)data, datalen);

      _pinfo.index += datalen;
    }
    else if ((datalen + _pinfo.index) == _pinfo.len)
    {
      _pstate = 0;

      if (_pinfo.opcode == WS_DISCONNECT)
      {
        if (datalen)
        {
          uint16_t reasonCode = (uint16_t)(data[0] << 8) + data[1];
          char * reasonString = (char*)(data + 2);

          if (reasonCode > 1001)
          {
            _server->_handleEvent(this, WS_EVT_ERROR, (void *)&reasonCode, (uint8_t*)reasonString, strlen(reasonString));
          }
        }

        if (_status == WS_DISCONNECTING)
        {
          _status = WS_DISCONNECTED;
          _client->close(true);
        }
        else
        {
          _status = WS_DISCONNECTING;
          _client->ackLater();
          _queueControl(new AsyncWebSocketControl(WS_DISCONNECT, data, datalen));
        }
      }
      else if (_pinfo.opcode == WS_PING)
      {
        _queueControl(new AsyncWebSocketControl(WS_PONG, data, datalen));
      }
      else if (_pinfo.opcode == WS_PONG)
      {
        if (datalen != AWSC_PING_PAYLOAD_LEN || memcmp(AWSC_PING_PAYLOAD, data, AWSC_PING_PAYLOAD_LEN) != 0)
          _server->_handleEvent(this, WS_EVT_PONG, NULL, data, datalen);
      }
      else if (_pinfo.opcode < 8)   //continuation or text/binary frame
      {
        _server->_handleEvent(this, WS_EVT_DATA, (void *)&_pinfo, data, datalen);
      }
    }
    else
    {
      LOGDEBUG3("Frame Error: len =", datalen, "index =", _pinfo.index);
      LOGDEBUG1("Frame Error: total =", _pinfo.len);

      //what should we do?
      break;
    }

    // restore byte as _handleEvent may have added a null terminator i.e., data[len] = 0;
    if (datalen > 0)
      data[datalen] = datalast;

    data += datalen;
    plen -= datalen;
  }
}

/////////////////////////////////////////////////

size_t AsyncWebSocketClient::printf(const char *format, ...)
{
  va_list arg;
  va_start(arg, format);
  char* temp = new char[MAX_PRINTF_LEN];

  if (!temp)
  {
    va_end(arg);

    return 0;
  }

  char* buffer = temp;
  size_t len = vsnprintf(temp, MAX_PRINTF_LEN, format, arg);
  va_end(arg);

  if (len > (MAX_PRINTF_LEN - 1))
  {
    buffer = new char[len + 1];

    if (!buffer)
    {
      delete[] temp;

      return 0;
    }

    va_start(arg, format);
    vsnprintf(buffer, len + 1, format, arg);
    va_end(arg);
  }

  text(buffer, len);

  if (buffer != temp)
  {
    delete[] buffer;
  }

  delete[] temp;

  return len;
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::text(const char * message, size_t len)
{
  _queueMessage(new AsyncWebSocketBasicMessage(message, len));
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::text(const char * message)
{
  text(message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::text(uint8_t * message, size_t len)
{
  text((const char *)message, len);
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::text(char * message)
{
  text(message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::text(const String & message)
{
  text(message.c_str(), message.length());
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::text(AsyncWebSocketMessageBuffer * buffer)
{
  _queueMessage(new AsyncWebSocketMultiMessage(buffer));
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::binary(const char * message, size_t len)
{
  _queueMessage(new AsyncWebSocketBasicMessage(message, len, WS_BINARY));
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::binary(const char * message)
{
  binary(message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::binary(uint8_t * message, size_t len)
{
  binary((const char *)message, len);
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::binary(char * message)
{
  binary(message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::binary(const String & message)
{
  binary(message.c_str(), message.length());
}

/////////////////////////////////////////////////

void AsyncWebSocketClient::binary(AsyncWebSocketMessageBuffer * buffer)
{
  _queueMessage(new AsyncWebSocketMultiMessage(buffer, WS_BINARY));
}

/////////////////////////////////////////////////

IPAddress AsyncWebSocketClient::remoteIP()
{
  if (!_client)
  {
    //return IPAddress(0U);
    return IPAddress(0, 0, 0, 0);
  }

  return _client->remoteIP();
}

/////////////////////////////////////////////////

uint16_t AsyncWebSocketClient::remotePort()
{
  if (!_client)
  {
    return 0;
  }

  return _client->remotePort();
}

/////////////////////////////////////////////////
/////////////////////////////////////////////////

/*
   Async Web Socket - Each separate socket location
*/

AsyncWebSocket::AsyncWebSocket(const String & url)
  : _url(url)
  , _clients(LinkedList<AsyncWebSocketClient * >([](AsyncWebSocketClient * c)
{
  delete c;
}))
, _cNextId(1), _enabled(true), _buffers(LinkedList<AsyncWebSocketMessageBuffer *>([](AsyncWebSocketMessageBuffer * b)
{
  delete b;
}))
{
  _eventHandler = NULL;
}

/////////////////////////////////////////////////

AsyncWebSocket::~AsyncWebSocket() {}

/////////////////////////////////////////////////

void AsyncWebSocket::_handleEvent(AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data,
                                  size_t len)
{
  if (_eventHandler != NULL)
  {
    _eventHandler(this, client, type, arg, data, len);
  }
}

/////////////////////////////////////////////////

void AsyncWebSocket::_addClient(AsyncWebSocketClient * client)
{
  _clients.add(client);
}

/////////////////////////////////////////////////

void AsyncWebSocket::_handleDisconnect(AsyncWebSocketClient * client)
{
  _clients.remove_first([ = ](AsyncWebSocketClient * c)
  {
    return c->id() == client->id();
  });
}

/////////////////////////////////////////////////

bool AsyncWebSocket::availableForWriteAll()
{
  for (const auto& c : _clients)
  {
    if (c->queueIsFull())
      return false;
  }

  return true;
}

/////////////////////////////////////////////////

bool AsyncWebSocket::availableForWrite(uint32_t id)
{
  for (const auto& c : _clients)
  {
    if (c->queueIsFull() && (c->id() == id ))
      return false;
  }

  return true;
}

/////////////////////////////////////////////////

size_t AsyncWebSocket::count() const
{
  return _clients.count_if([](AsyncWebSocketClient * c)
  {
    return c->status() == WS_CONNECTED;
  });
}

/////////////////////////////////////////////////

AsyncWebSocketClient * AsyncWebSocket::client(uint32_t id)
{
  for (const auto &c : _clients)
  {
    if (c->id() == id && c->status() == WS_CONNECTED)
    {
      return c;
    }
  }

  return nullptr;
}

/////////////////////////////////////////////////

void AsyncWebSocket::close(uint32_t id, uint16_t code, const char * message)
{
  AsyncWebSocketClient * c = client(id);

  if (c)
    c->close(code, message);
}

/////////////////////////////////////////////////

void AsyncWebSocket::closeAll(uint16_t code, const char * message)
{
  for (const auto& c : _clients)
  {
    if (c->status() == WS_CONNECTED)
      c->close(code, message);
  }
}

/////////////////////////////////////////////////

void AsyncWebSocket::cleanupClients(uint16_t maxClients)
{
  if (count() > maxClients)
  {
    _clients.front()->close();
  }
}

/////////////////////////////////////////////////

void AsyncWebSocket::ping(uint32_t id, uint8_t *data, size_t len)
{
  AsyncWebSocketClient * c = client(id);

  if (c)
    c->ping(data, len);
}

/////////////////////////////////////////////////

void AsyncWebSocket::pingAll(uint8_t *data, size_t len)
{
  for (const auto& c : _clients)
  {
    if (c->status() == WS_CONNECTED)
      c->ping(data, len);
  }
}

/////////////////////////////////////////////////

void AsyncWebSocket::text(uint32_t id, const char * message, size_t len)
{
  AsyncWebSocketClient * c = client(id);

  if (c)
    c->text(message, len);
}

/////////////////////////////////////////////////

void AsyncWebSocket::textAll(AsyncWebSocketMessageBuffer * buffer)
{
  if (!buffer)
    return;

  buffer->lock();

  for (const auto& c : _clients)
  {
    if (c->status() == WS_CONNECTED)
    {
      c->text(buffer);
    }
  }

  buffer->unlock();
  _cleanBuffers();
}

/////////////////////////////////////////////////

void AsyncWebSocket::textAll(const char * message, size_t len)
{
  AsyncWebSocketMessageBuffer * WSBuffer = makeBuffer((uint8_t *)message, len);
  textAll(WSBuffer);
}

/////////////////////////////////////////////////

void AsyncWebSocket::binary(uint32_t id, const char * message, size_t len)
{
  AsyncWebSocketClient * c = client(id);

  if (c)
    c->binary(message, len);
}

/////////////////////////////////////////////////

void AsyncWebSocket::binaryAll(const char * message, size_t len)
{
  AsyncWebSocketMessageBuffer * buffer = makeBuffer((uint8_t *)message, len);
  binaryAll(buffer);
}

/////////////////////////////////////////////////

void AsyncWebSocket::binaryAll(AsyncWebSocketMessageBuffer * buffer)
{
  if (!buffer)
    return;

  buffer->lock();

  for (const auto& c : _clients)
  {
    if (c->status() == WS_CONNECTED)
      c->binary(buffer);
  }

  buffer->unlock();
  _cleanBuffers();
}

/////////////////////////////////////////////////

void AsyncWebSocket::message(uint32_t id, AsyncWebSocketMessage * message)
{
  AsyncWebSocketClient * c = client(id);

  if (c)
    c->message(message);
}

/////////////////////////////////////////////////

void AsyncWebSocket::messageAll(AsyncWebSocketMultiMessage * message)
{
  for (const auto& c : _clients)
  {
    if (c->status() == WS_CONNECTED)
      c->message(message);
  }

  _cleanBuffers();
}

/////////////////////////////////////////////////

size_t AsyncWebSocket::printf(uint32_t id, const char *format, ...)
{
  AsyncWebSocketClient * c = client(id);

  if (c)
  {
    va_list arg;
    va_start(arg, format);
    size_t len = c->printf(format, arg);
    va_end(arg);

    return len;
  }

  return 0;
}

/////////////////////////////////////////////////

size_t AsyncWebSocket::printfAll(const char *format, ...)
{
  va_list arg;
  char* temp = new char[MAX_PRINTF_LEN];

  if (!temp)
  {
    return 0;
  }

  va_start(arg, format);
  size_t len = vsnprintf(temp, MAX_PRINTF_LEN, format, arg);
  va_end(arg);
  delete[] temp;

  AsyncWebSocketMessageBuffer * buffer = makeBuffer(len);

  if (!buffer)
  {
    return 0;
  }

  va_start(arg, format);
  vsnprintf( (char *)buffer->get(), len + 1, format, arg);
  va_end(arg);

  textAll(buffer);

  return len;
}

/////////////////////////////////////////////////

void AsyncWebSocket::text(uint32_t id, const char * message)
{
  text(id, message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocket::text(uint32_t id, uint8_t * message, size_t len)
{
  text(id, (const char *)message, len);
}

/////////////////////////////////////////////////

void AsyncWebSocket::text(uint32_t id, char * message)
{
  text(id, message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocket::text(uint32_t id, const String & message)
{
  text(id, message.c_str(), message.length());
}

/////////////////////////////////////////////////

void AsyncWebSocket::textAll(const char * message)
{
  textAll(message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocket::textAll(uint8_t * message, size_t len)
{
  textAll((const char *)message, len);
}

/////////////////////////////////////////////////

void AsyncWebSocket::textAll(char * message)
{
  textAll(message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocket::textAll(const String & message)
{
  textAll(message.c_str(), message.length());
}

/////////////////////////////////////////////////

void AsyncWebSocket::binary(uint32_t id, const char * message)
{
  binary(id, message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocket::binary(uint32_t id, uint8_t * message, size_t len)
{
  binary(id, (const char *)message, len);
}

/////////////////////////////////////////////////

void AsyncWebSocket::binary(uint32_t id, char * message)
{
  binary(id, message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocket::binary(uint32_t id, const String & message)
{
  binary(id, message.c_str(), message.length());
}

/////////////////////////////////////////////////

void AsyncWebSocket::binaryAll(const char * message)
{
  binaryAll(message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocket::binaryAll(uint8_t * message, size_t len)
{
  binaryAll((const char *)message, len);
}

/////////////////////////////////////////////////

void AsyncWebSocket::binaryAll(char * message)
{
  binaryAll(message, strlen(message));
}

/////////////////////////////////////////////////

void AsyncWebSocket::binaryAll(const String & message)
{
  binaryAll(message.c_str(), message.length());
}

/////////////////////////////////////////////////

const char * WS_STR_CONNECTION = "Connection";
const char * WS_STR_UPGRADE    = "Upgrade";
const char * WS_STR_ORIGIN     = "Origin";
const char * WS_STR_VERSION    = "Sec-WebSocket-Version";
const char * WS_STR_KEY        = "Sec-WebSocket-Key";
const char * WS_STR_PROTOCOL   = "Sec-WebSocket-Protocol";
const char * WS_STR_ACCEPT     = "Sec-WebSocket-Accept";
const char * WS_STR_UUID       = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/////////////////////////////////////////////////

bool AsyncWebSocket::canHandle(AsyncWebServerRequest * request)
{
  if (!_enabled)
    return false;

  if (request->method() != HTTP_GET || !request->url().equals(_url) || !request->isExpectedRequestedConnType(RCT_WS))
    return false;

  request->addInterestingHeader(WS_STR_CONNECTION);
  request->addInterestingHeader(WS_STR_UPGRADE);
  request->addInterestingHeader(WS_STR_ORIGIN);
  request->addInterestingHeader(WS_STR_VERSION);
  request->addInterestingHeader(WS_STR_KEY);
  request->addInterestingHeader(WS_STR_PROTOCOL);

  return true;
}

/////////////////////////////////////////////////

void AsyncWebSocket::handleRequest(AsyncWebServerRequest * request)
{
  if (!request->hasHeader(WS_STR_VERSION) || !request->hasHeader(WS_STR_KEY))
  {
    request->send(400);

    return;
  }

  if ((_username != "" && _password != "") && !request->authenticate(_username.c_str(), _password.c_str()))
  {
    return request->requestAuthentication();
  }

  AsyncWebHeader* version = request->getHeader(WS_STR_VERSION);

  if (version->value().toInt() != 13)
  {
    AsyncWebServerResponse *response = request->beginResponse(400);
    response->addHeader(WS_STR_VERSION, "13");
    request->send(response);

    return;
  }

  AsyncWebHeader* key = request->getHeader(WS_STR_KEY);
  AsyncWebServerResponse *response = new AsyncWebSocketResponse(key->value(), this);

  if (request->hasHeader(WS_STR_PROTOCOL))
  {
    AsyncWebHeader* protocol = request->getHeader(WS_STR_PROTOCOL);
    //ToDo: check protocol
    response->addHeader(WS_STR_PROTOCOL, protocol->value());
  }

  request->send(response);
}

/////////////////////////////////////////////////

AsyncWebSocketMessageBuffer * AsyncWebSocket::makeBuffer(size_t size)
{
  AsyncWebSocketMessageBuffer * buffer = new AsyncWebSocketMessageBuffer(size);

  if (buffer)
  {
    AsyncWebLockGuard l(_lock);
    _buffers.add(buffer);
  }

  return buffer;
}

/////////////////////////////////////////////////

AsyncWebSocketMessageBuffer * AsyncWebSocket::makeBuffer(uint8_t * data, size_t size)
{
  AsyncWebSocketMessageBuffer * buffer = new AsyncWebSocketMessageBuffer(data, size);

  if (buffer)
  {
    AsyncWebLockGuard l(_lock);
    _buffers.add(buffer);
  }

  return buffer;
}

/////////////////////////////////////////////////

void AsyncWebSocket::_cleanBuffers()
{
  AsyncWebLockGuard l(_lock);

  for (AsyncWebSocketMessageBuffer * c : _buffers)
  {
    if (c && c->canDelete())
    {
      _buffers.remove(c);
    }
  }
}

/////////////////////////////////////////////////

AsyncWebSocket::AsyncWebSocketClientLinkedList AsyncWebSocket::getClients() const
{
  return _clients;
}

/////////////////////////////////////////////////
/////////////////////////////////////////////////

/*
   Response to Web Socket request - sends the authorization and detaches the TCP Client from the web server
   Authentication code from https://github.com/Links2004/arduinoWebSockets/blob/master/src/WebSockets.cpp#L480
*/

AsyncWebSocketResponse::AsyncWebSocketResponse(const String & key, AsyncWebSocket * server)
{
  _server = server;
  _code = 101;
  _sendContentLength = false;

  uint8_t * hash = (uint8_t*) malloc(HASH_BUFFER_SIZE);

  if (hash == NULL)
  {
    _state = RESPONSE_FAILED;

    return;
  }

  char * buffer = (char *) malloc(33);

  if (buffer == NULL)
  {
    free(hash);
    _state = RESPONSE_FAILED;

    return;
  }

  LOGDEBUG1("key =", key.c_str());
  // KH, for Ethernet
  sha1_context _ctx;

  (String&) key += WS_STR_UUID;

  sha1_starts(&_ctx);
  sha1_update(&_ctx, (const unsigned char*) key.c_str(), key.length());
  sha1_finish(&_ctx, hash);
  //////  Mod by: TothTechnika!
  b64_encode((const unsigned char *)hash, 20, buffer);
  sprintf(buffer, "%s", trim(buffer));
  //////

  addHeader(WS_STR_CONNECTION, WS_STR_UPGRADE);
  addHeader(WS_STR_UPGRADE, "websocket");
  addHeader(WS_STR_ACCEPT, (String)buffer);

  free(buffer);
  free(hash);
}

/////////////////////////////////////////////////

void AsyncWebSocketResponse::_respond(AsyncWebServerRequest * request)
{
  if (_state == RESPONSE_FAILED)
  {
    request->client()->close(true);

    return;
  }

  String out = _assembleHead(request->version());
  request->client()->write(out.c_str(), _headLength, TCP_WRITE_FLAG_COPY);

  _state = RESPONSE_WAIT_ACK;
}

/////////////////////////////////////////////////

size_t AsyncWebSocketResponse::_ack(AsyncWebServerRequest * request, size_t len, uint32_t time)
{
  AWS_TEENSY41_UNUSED(time);

  if (len)
  {
    new AsyncWebSocketClient(request, _server);
  }

  return 0;
}
