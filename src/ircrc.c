#define _GNU_SOURCE

#include <stdlib.h>
#include <time.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_network.h>
#include <vlc_playlist.h>
#include <vlc_url.h>

#define MAX_LINE 8096
#define SEND_BUFFER_LEN 8096

struct circular_buffer {
  char *buffer;
  char *buffer_end;
  char *head;
  char *tail;
};

struct intf_sys_t
{
  int fd;
  int timeout;
  
  input_thread_t *input;
  vlc_thread_t thread;

  char *line;
  int line_loc;

  struct circular_buffer* send_buffer;
  int send_buffer_len;
  int send_buffer_loc;

  playlist_t *playlist;

  char *server, *channel, *nick;
};

struct irc_msg_t {
  char *prefix;
  char *command;
  char *params;
  char *trailing;
};


static int Open(vlc_object_t *);
static void Close(vlc_object_t *);
void EventLoop(int, void *);
int HandleRead(void *);
int HandleWrite(void *);
void LineReceived(void *, char *);
void irc_PING(void *, struct irc_msg_t *);
void irc_PRIVMSG(void *handle, struct irc_msg_t *irc_msg);
void SendBufferAppend(void *, char *);
void ResizeSendBuffer(void *);
int IndexOf(char *, char);
struct irc_msg_t *ParseIRC(char *);
void SendBufferInit(vlc_object_t *obj);
static void *Run(void *);
static int Playlist(vlc_object_t *, char const *, vlc_value_t, vlc_value_t, void *);
static void RegisterCallbacks(intf_thread_t *);
void FreeIRCMsg(struct irc_msg_t *irc_msg);
int SplitString(char* str, char *delim, char *chs[], int max_size);
void SendMessage(void *handle, char *message);
static input_item_t *parse_MRL( const char *mrl );

vlc_module_begin()
    set_shortname("IRC")
    set_description("IRC interface")
    set_capability("interface", 0)
    set_callbacks(Open, Close)
    set_category(CAT_INTERFACE)
    add_string("server", NULL, "server", "IRC server", true)
    add_string("channel", NULL, "channel", "IRC channel", true)
    add_string("nick", NULL, "nick", "IRC nickname", true)
vlc_module_end()

static int Open(vlc_object_t *obj)
{
    intf_thread_t *intf = (intf_thread_t *)obj;

    intf_sys_t *sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    intf->p_sys = sys;

    sys->server = var_InheritString(intf, "server");
    sys->channel = var_InheritString(intf, "channel");
    sys->nick = var_InheritString(intf, "nick");

    if(sys->server == NULL) {
      msg_Err(intf, "No server specified, use --server");
      return VLC_SUCCESS;
    }
    
    if(sys->channel == NULL) {
      msg_Err(intf, "No channel specified, use --channel");
      return VLC_SUCCESS;
    }

    if(sys->nick == NULL) {
      msg_Err(intf, "No nickname specified, use --nick");
      return VLC_SUCCESS;
    }

    if(vlc_clone(&sys->thread, Run, intf, VLC_THREAD_PRIORITY_LOW)) {
      return VLC_ENOMEM;
    }

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = intf->p_sys;

    msg_Dbg(intf, "Unloading IRC interface");

    free(sys);
}

static void *Run(void* handle)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;
  int fd;

  int canc = vlc_savecancel();

  RegisterCallbacks(intf);

  while(1) {
    msg_Info(intf, "Creating IRC connection...");

    fd = net_ConnectTCP(VLC_OBJECT(intf), sys->server, 6667);

    if(fd == -1) {
      msg_Err(intf, "Error connecting to server");
      return NULL;
    }

    msg_Info(intf, "Connected to server");

    /* initialize context */
    sys->fd = fd;
    sys->line_loc = 0;

    SendBufferInit((vlc_object_t*)intf);

    SendBufferAppend(intf, (char*)"NICK ");
    SendBufferAppend(intf, sys->nick);
    SendBufferAppend(intf, (char*)"\r\n");

    SendBufferAppend(intf, (char*)"USER ");
    SendBufferAppend(intf, sys->nick);
    SendBufferAppend(intf, (char*)" 8 * vlc\r\n");

    sys->playlist = pl_Get(intf);

    #ifdef STOP_HACK
    playlist_Pause(sys->playlist);
    input_thread_t * input = playlist_CurrentInput(sys->playlist);
    var_SetFloat(input, "position", 0.0);
    #endif

    EventLoop(fd, intf);

    free(sys->send_buffer->buffer);

    sleep(30);
  }

  free(sys);

  vlc_restorecancel(canc);

  return NULL;
}

void EventLoop(int fd, void *handle)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  sys->line = (char *)malloc(MAX_LINE * sizeof(char));

  while(1) {
    struct pollfd ufd = { .fd = fd, .events = POLLIN | POLLOUT, };
    
    if(poll(&ufd, 1, 1000) <= 0) /* block for 1s so we don't spin */
      continue;
    
    if(ufd.revents & POLLIN) {
      int rv = HandleRead(handle);
      if(rv != 0) {
	msg_Err(intf, "Read error: %s", strerror(rv));
	break;
      }
    } else if(ufd.revents & POLLOUT) {
      int rv = HandleWrite(handle);
      if(rv != 0) {
	msg_Err(intf, "Write error: %s", strerror(rv));
	break;
      }
    }
  }

  free(sys->line);
}

int HandleRead(void *handle) {
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  static char ch, pch;

  int rv = recv(sys->fd, &ch, 1, 0);
  if(rv == -1)
    return errno;
  else if(rv == 0)
    return -2;
  
  if(pch == '\r' && ch == '\n') { /* were the last two characters \r\n? */
    sys->line[sys->line_loc-1] = '\0'; /* overwrite CR with a nullbyte */
    LineReceived(handle, sys->line);
    sys->line_loc = 0;
    sys->line = (char *)malloc(MAX_LINE * sizeof(char)); /* allocate a new line, lineReceived will free the old one */
  } else {
    sys->line[sys->line_loc] = ch;
    pch = ch;
    sys->line_loc++;
  }

  return 0;
}

int HandleWrite(void *handle)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;
  
  struct circular_buffer* send_buffer = sys->send_buffer;

  size_t send_len = (send_buffer->tail > send_buffer->head) ? (SEND_BUFFER_LEN - (long)send_buffer->tail) : (send_buffer->head - send_buffer->tail);

  int sent = send(sys->fd, send_buffer->tail, send_len, 0);

  if(sent == -1)
    return errno;

  if(sent == 0)
    return 0;

  send_buffer->tail += sent;

  if(send_buffer->tail - send_buffer->buffer == SEND_BUFFER_LEN) {
    send_buffer->tail = send_buffer->buffer;
  }

  return 0;
}

void LineReceived(void *handle, char *line)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  msg_Dbg(intf, "Line received: %s", line);

  struct irc_msg_t *irc_msg = ParseIRC(line);

  if(irc_msg == NULL) {
    msg_Dbg(intf, "Malformed IRC message: %s", line);
    goto line_error;
  }
    
  if(strcmp(irc_msg->command, "376") == 0) { /* End of MotD */
    SendBufferAppend(handle, (char*)"JOIN ");
    SendBufferAppend(handle, sys->channel);
    SendBufferAppend(handle, (char*)"\r\n");
  } else if(strcmp(irc_msg->command, "PING") == 0) {
    irc_PING(handle, irc_msg);
  } else if(strcmp(irc_msg->command, "PRIVMSG") == 0) {
    irc_PRIVMSG(handle, irc_msg);
  }

 line_error:
  free(line);
  FreeIRCMsg(irc_msg);
  return;
}

void FreeIRCMsg(struct irc_msg_t *irc_msg)
{
  if(irc_msg) {
    if(irc_msg->prefix)
      free(irc_msg->prefix);
    if(irc_msg->command)
      free(irc_msg->command);
    if(irc_msg->params)
      free(irc_msg->params);
    if(irc_msg->trailing)
      free(irc_msg->trailing);
    free(irc_msg);
  }
}

void irc_PING(void *handle, struct irc_msg_t *irc_msg)
{
  SendBufferAppend(handle, (char*)"PONG :");
  SendBufferAppend(handle, irc_msg->trailing);
  SendBufferAppend(handle, (char*)"\r\n");
}

void irc_PRIVMSG(void *handle, struct irc_msg_t *irc_msg)
{
  intf_thread_t *intf = (intf_thread_t*)handle;

  char *msg = irc_msg->trailing;

  if(msg[0] == '!') {
    char *cmd = msg+1;
    char *tokens[2];
    int size = SplitString(cmd, (char*)" ", tokens, 2);
    char *psz_cmd = tokens[0];
    char *psz_arg;
    if (size < 1) {
      psz_arg = (char*)"";
    } else {
      psz_arg = tokens[1];
    }
    
    if(var_Type(intf, cmd) & VLC_VAR_ISCOMMAND) {
      vlc_value_t val;
      val.psz_string = psz_arg;
      if ((var_Type(intf, psz_cmd) & VLC_VAR_CLASS) == VLC_VAR_VOID) {
	var_TriggerCallback(intf, psz_cmd);
      } else { // STRING
	var_Set(intf, psz_cmd, val);
      }
    }
  }
}

int IndexOf(char *s, char d) {
  int offset = 0;
  while(s[offset] != d) {
    if(s[offset] == '\0') {
      return -1;
    }
    offset++;
  }
  return offset;
}

struct irc_msg_t *ParseIRC(char *line) {
  struct irc_msg_t *irc_msg = (struct irc_msg_t *)malloc(sizeof(struct irc_msg_t));
  irc_msg->prefix = irc_msg->command = irc_msg->params = irc_msg->trailing = NULL;

  int o = 0;
  
  if(line[0] == ':') { /* check for prefix */
    line++;

    o = IndexOf(line, ' ');
    if(o == -1) {
      return NULL;
    }

    irc_msg->prefix = (char*)malloc(sizeof(char) * (o + 1));
    strncpy(irc_msg->prefix, line, o);
    irc_msg->prefix[o] = '\0';      
    line += o + 1;
  }

  o = IndexOf(line, ' ');
  if(o == -1) {
    return NULL;
  }

  irc_msg->command = (char*)malloc(sizeof(char) * (o + 1));
  strncpy(irc_msg->command, line, o);
  irc_msg->command[o] = '\0';  

  line += o + 1;

  o = IndexOf(line, ':');
  if(o == -1) { /* No trailing */
    irc_msg->params = (char*)malloc(sizeof(char) * (strlen(line) + 1));
    strcpy(irc_msg->params, line);
  } else {
    irc_msg->params = (char*)malloc(sizeof(char) * (o + 1));
    strncpy(irc_msg->params, line, o);
    irc_msg->params[o] = '\0';
    line += o + 1;
    irc_msg->trailing = (char*)malloc(sizeof(char) * (strlen(line) + 1));
    strcpy(irc_msg->trailing, line);
  }

  return irc_msg;
}

static void RegisterCallbacks(intf_thread_t *intf)
{
  /* Register commands that will be cleaned up upon object destruction */
#define ADD( name, type, target )                                   \
  var_Create(intf, name, VLC_VAR_ ## type | VLC_VAR_ISCOMMAND ); \
  var_AddCallback(intf, name, target, NULL );
      ADD("play", VOID, Playlist)
      ADD("pause", VOID, Playlist)
      ADD("enqueue", STRING, Playlist)
      ADD("next", VOID, Playlist)
      ADD("prev", VOID, Playlist)
      ADD("clear", VOID, Playlist)
#undef ADD
}

static int Playlist(vlc_object_t *obj, char const *cmd,
                    vlc_value_t oldval, vlc_value_t newval, void *p_data)
{
  intf_thread_t *intf = (intf_thread_t*)obj;
  intf_sys_t *sys = intf->p_sys;

  playlist_t *playlist = sys->playlist;
  input_thread_t * input = playlist_CurrentInput(playlist);
  int state;

  if(input) {
    state = var_GetInteger(input, "state");
    vlc_object_release(input);
  } else {
    return VLC_EGENERIC;
  }

  if(strcmp(cmd, "pause") == 0) {
    msg_Info(intf, "Pause");    
    if(state == PLAYING_S) {
      playlist_Pause(sys->playlist);
      SendMessage(intf, "Paused");
    }
  } else if(strcmp(cmd, "play") == 0) {
    msg_Info(intf, "Play");
    if(state != PLAYING_S) {
      playlist_Play(sys->playlist);
      SendMessage(intf, "Playing");
    }
  } else if(strcmp(cmd, "enqueue") == 0 && newval.psz_string && *newval.psz_string) {
    input_item_t *p_item = parse_MRL(newval.psz_string);
    if(p_item) {
      msg_Info(intf,  "Trying to enqueue %s to playlist", newval.psz_string );
      if (playlist_AddInput(sys->playlist, p_item,
			    PLAYLIST_APPEND, PLAYLIST_END, true,
			    pl_Unlocked ) != VLC_SUCCESS) {
	return VLC_EGENERIC;
      }
      SendMessage(intf, "Enqueued");
    }
  } else if(strcmp(cmd, "next") == 0) {
    msg_Info(intf, "Next");
    SendMessage(intf, "Next");
    playlist_Next(sys->playlist);
  } else if(strcmp(cmd, "prev") == 0) {
    msg_Info(intf, "Prev");
    SendMessage(intf, "Next");    
    playlist_Prev(sys->playlist);
  } else if(strcmp(cmd, "clear") == 0) {
    msg_Info(intf, "Clear");
    SendMessage(intf, "Clear");
    playlist_Stop(sys->playlist);
    playlist_Clear(sys->playlist, pl_Unlocked);
  }

  return VLC_SUCCESS;
}

void SendBufferInit(vlc_object_t *obj)
{
  intf_thread_t *intf = (intf_thread_t*)obj;
  intf_sys_t *sys = intf->p_sys;

  sys->send_buffer = (struct circular_buffer*)malloc(sizeof(struct circular_buffer));

  struct circular_buffer *send_buffer = sys->send_buffer;

  send_buffer->buffer = (char*)malloc(SEND_BUFFER_LEN);
  send_buffer->buffer_end = (char*)send_buffer->buffer+SEND_BUFFER_LEN;
  send_buffer->head = send_buffer->buffer;
  send_buffer->tail = send_buffer->buffer;
}

void SendBufferAppend(void *handle, char *data)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  size_t data_len = strlen(data);

  struct circular_buffer *send_buffer = sys->send_buffer;

  if(send_buffer->head + data_len > send_buffer->buffer_end) {
    size_t copy_len = (send_buffer->buffer_end - send_buffer->tail);
    memcpy(send_buffer->head, data, copy_len);
    memcpy(send_buffer->buffer, data + copy_len, data_len - copy_len);
    send_buffer->head = send_buffer->buffer + (data_len - copy_len);
  } else {
    memcpy(send_buffer->head, data, data_len);
    send_buffer->head += data_len;
  }
}

int SplitString(char* str, char *delim, char *chs[], int max_size) {
  char *ch = strtok(str, delim);
  int i = 0;
  while(ch != NULL && i < max_size) {
    chs[i] = ch;
    ch = strtok(NULL, delim);
    i++;
  }
  return i;
}

void SendMessage(void *handle, char *message) {
    intf_thread_t *intf = (intf_thread_t*)handle;
    intf_sys_t *sys = intf->p_sys;
    SendBufferAppend(intf, (char*)"PRIVMSG ");
    SendBufferAppend(intf, sys->channel);
    SendBufferAppend(intf, (char*)" :");
    SendBufferAppend(intf, message);
    SendBufferAppend(intf, (char*)"\r\n");
}

/*
 * Shamelessly stolen from rc.c
 */

/*****************************************************************************
 * parse_MRL: build a input item from a full mrl
 *****************************************************************************
 * MRL format: "simplified-mrl [:option-name[=option-value]]"
 * We don't check for '"' or '\'', we just assume that a ':' that follows a
 * space is a new option. Should be good enough for our purpose.
 *****************************************************************************/
static input_item_t *parse_MRL( const char *mrl )
{
#define SKIPSPACE( p ) { while( *p == ' ' || *p == '\t' ) p++; }
#define SKIPTRAILINGSPACE( p, d ) \
    { char *e=d; while( e > p && (*(e-1)==' ' || *(e-1)=='\t') ){e--;*e=0;} }

    input_item_t *p_item = NULL;
    char *psz_item = NULL, *psz_item_mrl = NULL, *psz_orig, *psz_mrl;
    char **ppsz_options = NULL;
    int i, i_options = 0;

    if( !mrl ) return 0;

    psz_mrl = psz_orig = strdup( mrl );
    if( !psz_mrl )
        return NULL;
    while( *psz_mrl )
    {
        SKIPSPACE( psz_mrl );
        psz_item = psz_mrl;

        for( ; *psz_mrl; psz_mrl++ )
        {
            if( (*psz_mrl == ' ' || *psz_mrl == '\t') && psz_mrl[1] == ':' )
            {
                /* We have a complete item */
                break;
            }
            if( (*psz_mrl == ' ' || *psz_mrl == '\t') &&
                (psz_mrl[1] == '"' || psz_mrl[1] == '\'') && psz_mrl[2] == ':')
            {
                /* We have a complete item */
                break;
            }
        }

        if( *psz_mrl ) { *psz_mrl = 0; psz_mrl++; }
        SKIPTRAILINGSPACE( psz_item, psz_item + strlen( psz_item ) );

        /* Remove '"' and '\'' if necessary */
        if( *psz_item == '"' && psz_item[strlen(psz_item)-1] == '"' )
        { psz_item++; psz_item[strlen(psz_item)-1] = 0; }
        if( *psz_item == '\'' && psz_item[strlen(psz_item)-1] == '\'' )
        { psz_item++; psz_item[strlen(psz_item)-1] = 0; }

        if( !psz_item_mrl )
        {
            if( strstr( psz_item, "://" ) != NULL )
                psz_item_mrl = strdup( psz_item );
            else
                psz_item_mrl = vlc_path2uri( psz_item, NULL );
            if( psz_item_mrl == NULL )
            {
                free( psz_orig );
                return NULL;
            }
        }
        else if( *psz_item )
        {
            i_options++;
            ppsz_options = xrealloc( ppsz_options, i_options * sizeof(char *) );
            ppsz_options[i_options - 1] = &psz_item[1];
        }

        if( *psz_mrl ) SKIPSPACE( psz_mrl );
    }

    /* Now create a playlist item */
    if( psz_item_mrl )
    {
        p_item = input_item_New( psz_item_mrl, NULL );
        for( i = 0; i < i_options; i++ )
        {
            input_item_AddOption( p_item, ppsz_options[i], VLC_INPUT_OPTION_TRUSTED );
        }
        free( psz_item_mrl );
    }

    if( i_options ) free( ppsz_options );
    free( psz_orig );

    return p_item;
}
