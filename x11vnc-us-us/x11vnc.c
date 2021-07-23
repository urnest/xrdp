/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2004-2015
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * libvnc
 *
 * The message definitions used in this source file can be found mostly
 * in RFC6143 - "The Remote Framebuffer Protocol".
 *
 * The ExtendedDesktopSize encoding is reserved in RFC6143, but not
 * documented there. It is documented by the RFB protocol community
 * wiki currently held at https://github.com/rfbproto/rfbroto. This is
 * referred to below as the "RFB community wiki"
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include "../vnc/vnc.h"
#include "log.h"
#include "trans.h"
#include "ssl_calls.h"
#include "string_calls.h"
#include "xrdp_client_info.h"
#include <stdio.h>
#include <strings.h>

typedef enum
{
  X11_VNC_KEY_VALID=0x01,
  X11_VNC_KEY_AUTO_REPEAT=0x02,
  X11_VNC_KEY_IS_DOWN=0x04,
  X11_VNC_KEY_CAPS_LOCKABLE=0x08,
  X11_VNC_KEY_NUM_LOCKABLE=0x10,
  X11_VNC_KEY_IS_CAPSLOCK=0x20,
  X11_VNC_KEY_IS_NUMLOCK=0x40,
} X11VncKeyAttrs;

typedef enum
{
  X11_VNC_KEY_RELEASED=0,
  X11_VNC_KEY_PRESSED=1
} X11VncKeyDirection;

struct x11vnckey
{
  X11VncKeyAttrs attrs; // X11_VNC_KEY_* per rdp keycode
  uint8_t        vncKeyCode; // if appropriate based on keyState
  uint8_t        shiftedVncKeyCode;  // if appropriate based on keyState
};

static int keyAutoRepeats(const struct x11vnckey* const k)
{
  return k->attrs & X11_VNC_KEY_AUTO_REPEAT;
}
static int keyIsCapsLockable(const struct x11vnckey* const k)
{
  return k->attrs & X11_VNC_KEY_CAPS_LOCKABLE;
}
static int keyIsNumLockable(const struct x11vnckey* const k)
{
  return k->attrs & X11_VNC_KEY_NUM_LOCKABLE;
}
static int keyIsCapsLock(const struct x11vnckey* const k)
{
  return k->attrs & X11_VNC_KEY_IS_CAPSLOCK;
}
static int keyIsNumLock(const struct x11vnckey* const k)
{
  return k->attrs & X11_VNC_KEY_IS_NUMLOCK;
}
static int keyIsDown(const struct x11vnckey* const k)
{
  return k->attrs & X11_VNC_KEY_IS_DOWN;
}
static void setKeyDown(struct x11vnckey* const k)
{
  k->attrs |= X11_VNC_KEY_IS_DOWN;
}
static void setKeyUp(struct x11vnckey* const k)
{
  k->attrs &= ~X11_VNC_KEY_IS_DOWN;
}
struct x11vnc
{
  struct vnc _;
  struct x11vnckey keys[256];
  int    capsLocked;
  int    numLocked;
};

/* Client-to-server messages */
enum c2s
{
    C2S_SET_PIXEL_FORMAT = 0,
    C2S_SET_ENCODINGS = 2,
    C2S_FRAMEBUFFER_UPDATE_REQUEST = 3,
    C2S_KEY_EVENT = 4,
    C2S_POINTER_EVENT = 5,
    C2S_CLIENT_CUT_TEXT = 6,
};

/* Server to client messages */
enum s2c
{
    S2C_FRAMEBUFFER_UPDATE = 0,
    S2C_SET_COLOUR_MAP_ENTRIES = 1,
    S2C_BELL = 2,
    S2C_SERVER_CUT_TEXT = 3
};

/* Encodings and pseudo-encodings
 *
 * The RFC uses a signed type for these. We use an unsigned type as the
 * binary representation for the negative values is standardised in C
 * (which it wouldn't be for an enum value)
 */

typedef uint32_t encoding_type;

#define ENC_RAW                   (encoding_type)0
#define ENC_COPY_RECT             (encoding_type)1
#define ENC_CURSOR                (encoding_type)-239
#define ENC_DESKTOP_SIZE          (encoding_type)-223
#define ENC_EXTENDED_DESKTOP_SIZE (encoding_type)-308

/* Messages for ExtendedDesktopSize status code */
static const char *eds_status_msg[] =
{
    /* 0 */ "No error",
    /* 1 */ "Resize is administratively prohibited",
    /* 2 */ "Out of resources",
    /* 3 */ "Invalid screen layout",
    /* others */ "Unknown code"
};

/* elements in above list */
#define EDS_STATUS_MSG_COUNT \
    (sizeof(eds_status_msg) / sizeof(eds_status_msg[0]))

/* Used by enabled_encodings_mask */
enum
{
    MSK_EXTENDED_DESKTOP_SIZE = (1 << 0)
};

static int
lib_mod_process_message(struct vnc *v, struct stream *s);

/******************************************************************************/
static int
lib_send_copy(struct vnc *v, struct stream *s)
{
    return trans_write_copy_s(v->trans, s);
}

/******************************************************************************/
/* taken from vncauth.c */
/* performing the des3 crypt on the password so it can not be seen
   on the wire
   'bytes' in, contains 16 bytes server random
           out, random and 'passwd' conbined */
static void
rfbEncryptBytes(char *bytes, const char *passwd)
{
    char key[24];
    void *des;
    int len;

    /* key is simply password padded with nulls */
    g_memset(key, 0, sizeof(key));
    len = MIN(g_strlen(passwd), 8);
    g_mirror_memcpy(key, passwd, len);
    des = ssl_des3_encrypt_info_create(key, 0);
    ssl_des3_encrypt(des, 8, bytes, bytes);
    ssl_des3_info_delete(des);
    des = ssl_des3_encrypt_info_create(key, 0);
    ssl_des3_encrypt(des, 8, bytes + 8, bytes + 8);
    ssl_des3_info_delete(des);
}

/******************************************************************************/
/* sha1 hash 'passwd', create a string from the hash and call rfbEncryptBytes */
static void
rfbHashEncryptBytes(char *bytes, const char *passwd)
{
    char passwd_hash[20];
    char passwd_hash_text[40];
    void *sha1;
    int passwd_bytes;

    /* create password hash from password */
    passwd_bytes = g_strlen(passwd);
    sha1 = ssl_sha1_info_create();
    ssl_sha1_transform(sha1, "xrdp_vnc", 8);
    ssl_sha1_transform(sha1, passwd, passwd_bytes);
    ssl_sha1_transform(sha1, passwd, passwd_bytes);
    ssl_sha1_complete(sha1, passwd_hash);
    ssl_sha1_info_delete(sha1);
    g_snprintf(passwd_hash_text, 39, "%2.2x%2.2x%2.2x%2.2x",
               (tui8)passwd_hash[0], (tui8)passwd_hash[1],
               (tui8)passwd_hash[2], (tui8)passwd_hash[3]);
    passwd_hash_text[39] = 0;
    rfbEncryptBytes(bytes, passwd_hash_text);
}

/******************************************************************************/
static int
lib_process_channel_data(struct vnc *v, int chanid, int flags, int size,
                         struct stream *s, int total_size)
{
    int type;
    int status;
    int length;
    int clip_bytes;
    int index;
    int format;
    struct stream *out_s;

    if (chanid == v->clip_chanid)
    {
        in_uint16_le(s, type);
        in_uint16_le(s, status);
        in_uint32_le(s, length);

        LOG(LOG_LEVEL_DEBUG, "clip data type %d status %d length %d", type, status, length);
        LOG_DEVEL_HEXDUMP(LOG_LEVEL_TRACE, "clipboard data", s->p, s->end - s->p);
        switch (type)
        {
            case 2: /* CLIPRDR_FORMAT_ANNOUNCE */
                LOG(LOG_LEVEL_DEBUG, "CLIPRDR_FORMAT_ANNOUNCE - "
                    "status %d length %d", status, length);
                make_stream(out_s);
                init_stream(out_s, 8192);
                out_uint16_le(out_s, 3); // msg-type:  CLIPRDR_FORMAT_ACK
                out_uint16_le(out_s, 1); // msg-status-code:  CLIPRDR_RESPONSE
                out_uint32_le(out_s, 0); // null (?)
                out_uint8s(out_s, 4);    // pad
                s_mark_end(out_s);
                length = (int)(out_s->end - out_s->data);
                v->server_send_to_channel(v, v->clip_chanid, out_s->data,
                                          length, length, 3);
                free_stream(out_s);
#if 0
                // Send the CLIPRDR_DATA_REQUEST message to the cliprdr channel.
                //
                make_stream(out_s);
                init_stream(out_s, 8192);
                out_uint16_le(out_s, 4); // msg-type:  CLIPRDR_DATA_REQUEST
                out_uint16_le(out_s, 0); // msg-status-code:  CLIPRDR_REQUEST
                out_uint32_le(out_s, 4); // sizeof supported-format-list.
                out_uint32_le(out_s, 1); // supported-format-list:  CF_TEXT
                out_uint8s(out_s, 0);    // pad (garbage pad?)
                s_mark_end(out_s);
                length = (int)(out_s->end - out_s->data);
                v->server_send_to_channel(v, v->clip_chanid, out_s->data,
                                          length, length, 3);
                free_stream(out_s);
#endif
                break;

            case 3: /* CLIPRDR_FORMAT_ACK */
                LOG(LOG_LEVEL_DEBUG, "CLIPRDR_FORMAT_ACK - "
                    "status %d length %d", status, length);
                break;
            case 4: /* CLIPRDR_DATA_REQUEST */
                LOG(LOG_LEVEL_DEBUG, "CLIPRDR_DATA_REQUEST - "
                    "status %d length %d", status, length);
                format = 0;

                if (length >= 4)
                {
                    in_uint32_le(s, format);
                }

                /* only support CF_TEXT and CF_UNICODETEXT */
                if ((format != 1) && (format != 13))
                {
                    break;
                }

                make_stream(out_s);
                init_stream(out_s, 8192);
                out_uint16_le(out_s, 5);
                out_uint16_le(out_s, 1);

                if (format == 13) /* CF_UNICODETEXT */
                {
                    out_uint32_le(out_s, v->clip_data_s->size * 2 + 2);

                    for (index = 0; index < v->clip_data_s->size; index++)
                    {
                        out_uint8(out_s, v->clip_data_s->data[index]);
                        out_uint8(out_s, 0);
                    }

                    out_uint8s(out_s, 2);
                }
                else if (format == 1) /* CF_TEXT */
                {
                    out_uint32_le(out_s, v->clip_data_s->size + 1);

                    for (index = 0; index < v->clip_data_s->size; index++)
                    {
                        out_uint8(out_s, v->clip_data_s->data[index]);
                    }

                    out_uint8s(out_s, 1);
                }

                out_uint8s(out_s, 4); /* pad */
                s_mark_end(out_s);
                length = (int)(out_s->end - out_s->data);
                v->server_send_to_channel(v, v->clip_chanid, out_s->data, length,
                                          length, 3);
                free_stream(out_s);
                break;

            case 5: /* CLIPRDR_DATA_RESPONSE */
                LOG(LOG_LEVEL_DEBUG, "CLIPRDR_DATA_RESPONSE - "
                    "status %d length %d", status, length);
                clip_bytes = MIN(length, 256);
                // - Read the response data from the cliprdr channel, stream 's'.
                // - Send the response data to the vnc server, stream 'out_s'.
                //
                make_stream(out_s);
                // Send the RFB message type (CLIENT_CUT_TEXT) to the vnc server.
                init_stream(out_s, clip_bytes + 1 + 3 + 4 + 16);
                out_uint8(out_s, C2S_CLIENT_CUT_TEXT);
                out_uint8s(out_s, 3);  // padding
                // Send the length of the cut-text to  the vnc server.
                out_uint32_be(out_s, clip_bytes);
                // Send the cut-text (as read from 's') to the vnc server.
                for (index = 0; index < clip_bytes; index++)
                {
                    char cur_char = '\0';
                    in_uint8(s, cur_char);      // text in from 's'
                    out_uint8(out_s, cur_char); // text out to 'out_s'
                }
                s_mark_end(out_s);
                lib_send_copy(v, out_s);
                free_stream(out_s);
                break;

            default:
            {
                LOG(LOG_LEVEL_DEBUG, "VNC clip information unhandled");
                break;
            }
        }
    }
    else
    {
        LOG(LOG_LEVEL_DEBUG, "lib_process_channel_data: unknown chanid: "
            "%d :(v->clip_chanid) %d", chanid, v->clip_chanid);
    }

    return 0;
}

/**************************************************************************//**
 * Logs a debug message containing a screen layout
 *
 * @param lvl Level to log at
 * @param source Where the layout came from
 * @param layout Layout to log
 */
static void
log_screen_layout(const enum logLevels lvl, const char *source,
                  const struct vnc_screen_layout *layout)
{
    unsigned int i;
    char text[256];
    size_t pos;
    int res;

    pos = 0;
    res = g_snprintf(text, sizeof(text) - pos,
                     "Layout from %s (geom=%dx%d #screens=%u) :",
                     source, layout->total_width, layout->total_height,
                     layout->count);

    i = 0;
    while (res > 0 && (size_t)res < sizeof(text) - pos && i < layout->count)
    {
        pos += res;
        res = g_snprintf(&text[pos], sizeof(text) - pos,
                         " %d:(%dx%d+%d+%d)",
                         layout->s[i].id,
                         layout->s[i].width, layout->s[i].height,
                         layout->s[i].x, layout->s[i].y);
        ++i;
    }
    LOG(lvl, "%s", text);
}

/**************************************************************************//**
 * Compares two vnc_screen structures
 *
 * @param a First structure
 * @param b Second structure
 *
 * @return Suitable for sorting structures with ID as the primary key
 */
static int cmp_vnc_screen(const struct vnc_screen *a,
                          const struct vnc_screen *b)
{
    int result = 0;
    if (a->id != b->id)
    {
        result = a->id - b->id;
    }
    else if (a->x != b->x)
    {
        result = a->x - b->x;
    }
    else if (a->y != b->y)
    {
        result = a->y - b->y;
    }
    else if (a->width != b->width)
    {
        result = a->width - b->width;
    }
    else if (a->height != b->height)
    {
        result = a->height - b->height;
    }

    return result;
}

/**************************************************************************//**
 * Compares two vnc_screen_layout structures for equality
 * @param a First layout
 * @param b First layout
 * @return != 0 if structures are equal
 */
static int vnc_screen_layouts_equal(const struct vnc_screen_layout *a,
                                    const struct vnc_screen_layout *b)
{
    unsigned int i;
    int result = (a->total_width == b->total_width &&
                  a->total_height == b->total_height &&
                  a->count == b->count);
    if (result)
    {
        for (i = 0 ; result && i < a->count ; ++i)
        {
            result = (cmp_vnc_screen(&a->s[i], &b->s[i]) == 0);
        }
    }

    return result;
}

/**************************************************************************//**
 * Reads an extended desktop size rectangle from the VNC server
 *
 * @param v VNC object
 * @param [out] layout Desired layout for server
 * @return != 0 for error
 *
 * @pre The next octet read from v->trans is the number of screens
 * @pre layout is not already allocated
 *
 * @post if call is successful, layout->s must be freed after use.
 * @post Returned structure is in increasing ID order
 * @post layout->total_width is untouched
 * @post layout->total_height is untouched
 */
static int
read_extended_desktop_size_rect(struct vnc *v,
                                struct vnc_screen_layout *layout)
{
    struct stream *s;
    int error;
    unsigned int count;
    struct vnc_screen *screens;

    layout->count = 0;
    layout->s = NULL;

    make_stream(s);
    init_stream(s, 8192);

    /* Read in the current screen config */
    error = trans_force_read_s(v->trans, s, 4);
    if (error == 0)
    {
        /* Get the number of screens */
        in_uint8(s, count);
        in_uint8s(s, 3);

        error = trans_force_read_s(v->trans, s, 16 * count);
        if (error == 0)
        {
            screens = g_new(struct vnc_screen, count);
            if (screens == NULL)
            {
                LOG(LOG_LEVEL_ERROR,
                    "VNC : Can't alloc for %d screens", count);
                error = 1;
            }
            else
            {
                unsigned int i;
                for (i = 0 ; i < count ; ++i)
                {
                    in_uint32_be(s, screens[i].id);
                    in_uint16_be(s, screens[i].x);
                    in_uint16_be(s, screens[i].y);
                    in_uint16_be(s, screens[i].width);
                    in_uint16_be(s, screens[i].height);
                    in_uint32_be(s, screens[i].flags);
                }

                /* sort monitors in increasing ID order */
                qsort(screens, count, sizeof(screens[0]),
                      (int (*)(const void *, const void *))cmp_vnc_screen);
            }
        }
    }

    free_stream(s);

    if (error == 0)
    {
        layout->count = count;
        layout->s = screens;
    }

    return error;
}

/**************************************************************************//**
 * Sends a SetDesktopSize message
 *
 * @param v VNC object
 * @param layout Desired layout for server
 * @return != 0 for error
 *
 * The SetDesktopSize message is documented in the RFB community wiki
 * "SetDesktopSize" section.
 */
static int
send_set_desktop_size(struct vnc *v, const struct vnc_screen_layout *layout)
{
    unsigned int i;
    struct stream *s;
    int error;

    make_stream(s);
    init_stream(s, 8192);
    out_uint8(s, 251);
    out_uint8(s, 0);
    out_uint16_be(s, layout->total_width);
    out_uint16_be(s, layout->total_height);

    out_uint8(s, layout->count);
    out_uint8(s, 0);
    for (i = 0 ; i < layout->count ; ++i)
    {
        out_uint32_be(s, layout->s[i].id);
        out_uint16_be(s, layout->s[i].x);
        out_uint16_be(s, layout->s[i].y);
        out_uint16_be(s, layout->s[i].width);
        out_uint16_be(s, layout->s[i].height);
        out_uint32_be(s, layout->s[i].flags);
    }
    s_mark_end(s);
    LOG(LOG_LEVEL_DEBUG, "VNC Sending SetDesktopSize");
    error = lib_send_copy(v, s);
    free_stream(s);

    return error;
}

/**************************************************************************//**
 * Sets up a single-screen vnc_screen_layout structure
 *
 * @param layout Structure to set up
 * @param width New client width
 * @param height New client height
 *
 * @pre layout->count must be valid
 * @pre layout->s must be valid
 */
static void
set_single_screen_layout(struct vnc_screen_layout *layout,
                         int width, int height)
{
    int id = 0;
    int flags = 0;

    layout->total_width = width;
    layout->total_height = height;

    if (layout->count == 0)
    {
        /* No previous layout */
        layout->s = g_new(struct vnc_screen, 1);
    }
    else
    {
        /* Keep the ID and flags from the previous first screen */
        id = layout->s[0].id;
        flags = layout->s[0].flags;

        if (layout->count > 1)
        {
            g_free(layout->s);
            layout->s = g_new(struct vnc_screen, 1);
        }
    }
    layout->count = 1;
    layout->s[0].id = id;
    layout->s[0].x = 0;
    layout->s[0].y = 0;
    layout->s[0].width = width;
    layout->s[0].height = height;
    layout->s[0].flags = flags;
}

/**************************************************************************//**
 * Resize the client as a single screen
 *
 * @param v VNC object
 * @param update_in_progress True if there's a painter update in progress
 * @param width New client width
 * @param height New client height
 * @return != 0 for error
 *
 * The new client layout is recorded in v->client_layout. If the client was
 * multi-screen before this call, it won't be afterwards.
 */
static int
resize_client(struct vnc *v, int update_in_progress, int width, int height)
{
    int error = 0;

    if (v->client_layout.count != 1 ||
            v->client_layout.total_width != width ||
            v->client_layout.total_height != height)
    {
        if (update_in_progress)
        {
            error = v->server_end_update(v);
        }

        if (error == 0)
        {
            error = v->server_reset(v, width, height, v->server_bpp);
            if (error == 0)
            {
                set_single_screen_layout(&v->client_layout, width, height);
                if (update_in_progress)
                {
                    error = v->server_begin_update(v);
                }
            }
        }
    }

    return error;
}


/**************************************************************************//**
 * Resize the attached client from a layout
 *
 * @param v VNC object
 * @param update_in_progress True if there's a painter update in progress
 * @param layout Desired layout from server
 * @return != 0 for error
 *
 * This has some limitations. We have no way to move multiple screens about
 * on a connected client, and so we are not able to change the client unless
 * we're changing to a single screeen layout.
 */
static int
resize_client_from_layout(struct vnc *v,
                          int update_in_progress,
                          const struct vnc_screen_layout *layout)
{
    int error = 0;

    if (!vnc_screen_layouts_equal(&v->client_layout, layout))
    {
        /*
         * we don't have the capability to resize to anything other
         * than a single screen.
         */
        if (layout->count != 1)
        {
            LOG(LOG_LEVEL_ERROR,
                "VNC Resize to %d screen(s) from %d screen(s) "
                "not implemented",
                v->client_layout.count, layout->count);

            /* Dump some useful info, in case we get here when we don't
             * need to */
            log_screen_layout(LOG_LEVEL_ERROR, "OldLayout", &v->client_layout);
            log_screen_layout(LOG_LEVEL_ERROR, "NewLayout", layout);
            error = 1;
        }
        else
        {
            error = resize_client(v,
                                  update_in_progress,
                                  layout->total_width,
                                  layout->total_height);
        }
    }

    return error;
}

static int sendVncKey(struct vnc *v, int vncKeyCode, int pressed)
{
  struct stream *s;
  int error;
  make_stream(s);
  init_stream(s, 8192);
  out_uint8(s, C2S_KEY_EVENT);
  out_uint8(s, pressed); /* down flag */
  out_uint8s(s, 2);
  out_uint32_be(s, vncKeyCode);
  s_mark_end(s);
  error = lib_send_copy(v, s);
  return error;
}

/* translate vncKey into X11 key sym to send to vnc server */
/* pre: vncKey->attrs & X11_VNC_KEY_VALID */
static int translateVncKeyToX11KeySym(
  const struct x11vnckey* const vncKey,
  const int shiftIsDown,
  const int capsLocked,
  const int numLocked)
{
  if (keyIsCapsLockable(vncKey)){
    return (shiftIsDown != capsLocked)?
      vncKey->shiftedVncKeyCode:
      vncKey->vncKeyCode;
  }
  if (keyIsNumLockable(vncKey)){
    return (shiftIsDown != numLocked)?
      vncKey->shiftedVncKeyCode:
      vncKey->vncKeyCode;
  }
  return shiftIsDown?
    vncKey->shiftedVncKeyCode:
    vncKey->vncKeyCode;
}

/* pre: vncKey->attrs & X11_VNC_KEY_VALID */
/* returns 0 for success */
static int handleVncKeyPress(struct x11vnc* const v,
                             struct x11vnckey* const vncKey)
{
  const struct x11vnckey* keys=v->keys;
  const int shiftIsDown = keys[42].attrs & X11_VNC_KEY_IS_DOWN;
  int x11keySym=translateVncKeyToX11KeySym(
    vncKey,shiftIsDown,v->capsLocked,v->numLocked);
  int status=0;

  if (keyAutoRepeats(vncKey))
  {
    // rdp sends repeated key-down with no intervening key-up for
    // auto-repeat, so for keys we want to auto-repeat, we ignore
    // rdp key-up and instead generate a down-up pair for each rdp key-down
    // this way auto-repeat does not depend on network latency
    status=sendVncKey(&v->_, x11keySym, X11_VNC_KEY_PRESSED);
    if (status == 0){
      status=sendVncKey(&v->_, x11keySym, X11_VNC_KEY_RELEASED);
    }
  }
  else
  {
    // for non-autorepeat keys, ignore rdp's repeated key-downs with
    // no intervening key-up
    if (!keyIsDown(vncKey))
    {
      status = sendVncKey(&v->_, x11keySym, X11_VNC_KEY_PRESSED);
      if (status == 0){
        setKeyDown(vncKey);
      }
    }
  }
  return status;
}

/* pre: vncKey->attrs & X11_VNC_KEY_VALID */
/* returns 0 for success */
static int handleVncKeyRelease(struct x11vnc* const v,
                               struct x11vnckey* const vncKey)
{
  const struct x11vnckey* keys=v->keys;
  const int shiftIsDown = keys[42].attrs & X11_VNC_KEY_IS_DOWN;
  int x11keySym=translateVncKeyToX11KeySym(
    vncKey,shiftIsDown,v->capsLocked,v->numLocked);
  int status=0;

  if (keyIsCapsLock(vncKey))
  {
    v->capsLocked=!v->capsLocked;
  }
  if (keyIsNumLock(vncKey))
  {
    v->numLocked=!v->numLocked;
  }

  if (keyAutoRepeats(vncKey))
  {
    // rdp sends repeated key-down with no intervening key-up for
    // auto-repeat, so for keys we want to auto-repeat, we ignore
    // rdp key-up and instead generate a down-up pair for each rdp key-down
    // this way auto-repeat does not depend on network latency
  }
  else
  {
    // for non-autorepeat keys, ignore rdp's repeated key-downs with
    // no intervening key-up
    if (keyIsDown(vncKey))
    {
      status = sendVncKey(&v->_, x11keySym, X11_VNC_KEY_RELEASED);
      if (status == 0){
        setKeyUp(vncKey);
      }
    }
  }
  return status;
}

int lib_mod_handle_key(struct vnc *v_, int rdpKeyCode, int rdpKeyEvent)
{
  struct x11vnc* const v=(struct x11vnc*)v_;
  int status=0;
  const X11VncKeyDirection direction = (rdpKeyEvent==32768 ?
                                        X11_VNC_KEY_RELEASED :
                                        X11_VNC_KEY_PRESSED);
  if (rdpKeyCode >= 256)
  {
    printf("rdp key code %d (>= 256) is invalid for xrdp x11vnc module", rdpKeyCode);
    return 0;
  }
  if (rdpKeyCode < 0)
  {
    printf("rdp key code %d (< 0) is invalid for xrdp x11vnc module", rdpKeyCode);
    return 0;
  }
  if (! (v->keys[rdpKeyCode].attrs & X11_VNC_KEY_VALID))
  {
    printf("rdp key code %d is not mapped by xrdp x11vnc module", rdpKeyCode);
    return 0;
  }
  if (direction==X11_VNC_KEY_PRESSED){
    status = handleVncKeyPress(v,
                               &v->keys[rdpKeyCode]);
  }
  else{
    status = handleVncKeyRelease(v,
                                 &v->keys[rdpKeyCode]);
  }  
  return status;
}


/*****************************************************************************/
int
lib_mod_event(struct vnc *v, int msg, long param1, long param2,
              long param3, long param4)
{
    struct stream *s;
    int error;
    int x;
    int y;
    int cx;
    int cy;
    int size;
    int total_size;
    int chanid;
    int flags;
    char *data;

    error = 0;
    make_stream(s);

    if (msg == 0x5555) /* channel data */
    {
        chanid = LOWORD(param1);
        flags = HIWORD(param1);
        size = (int)param2;
        data = (char *)param3;
        total_size = (int)param4;

        if ((size >= 0) && (size <= (32 * 1024)) && (data != 0))
        {
            init_stream(s, size);
            out_uint8a(s, data, size);
            s_mark_end(s);
            s->p = s->data;
            error = lib_process_channel_data(v, chanid, flags, size, s, total_size);
        }
        else
        {
            error = 1;
        }
    }
    else if ((msg >= 15) && (msg <= 16)) /* key events */
    {
      // see lib_mod_handle_key
    }
    else if (msg >= 100 && msg <= 110) /* mouse events */
    {
        switch (msg)
        {
            case 100:
                break; /* WM_MOUSEMOVE */
            case 101:
                v->mod_mouse_state &= ~1;
                break; /* WM_LBUTTONUP */
            case 102:
                v->mod_mouse_state |= 1;
                break; /* WM_LBUTTONDOWN */
            case 103:
                v->mod_mouse_state &= ~4;
                break; /* WM_RBUTTONUP */
            case 104:
                v->mod_mouse_state |= 4;
                break; /* WM_RBUTTONDOWN */
            case 105:
                v->mod_mouse_state &= ~2;
                break;
            case 106:
                v->mod_mouse_state |= 2;
                break;
            case 107:
                v->mod_mouse_state &= ~8;
                break;
            case 108:
                v->mod_mouse_state |= 8;
                break;
            case 109:
                v->mod_mouse_state &= ~16;
                break;
            case 110:
                v->mod_mouse_state |= 16;
                break;
        }

        init_stream(s, 8192);
        out_uint8(s, C2S_POINTER_EVENT);
        out_uint8(s, v->mod_mouse_state);
        out_uint16_be(s, param1);
        out_uint16_be(s, param2);
        s_mark_end(s);
        error = lib_send_copy(v, s);
    }
    else if (msg == 200) /* invalidate */
    {
        if (v->suppress_output == 0)
        {
            init_stream(s, 8192);
            out_uint8(s, C2S_FRAMEBUFFER_UPDATE_REQUEST);
            out_uint8(s, 0); /* incremental == 0 : Full contents */
            x = (param1 >> 16) & 0xffff;
            out_uint16_be(s, x);
            y = param1 & 0xffff;
            out_uint16_be(s, y);
            cx = (param2 >> 16) & 0xffff;
            out_uint16_be(s, cx);
            cy = param2 & 0xffff;
            out_uint16_be(s, cy);
            s_mark_end(s);
            error = lib_send_copy(v, s);
        }
    }

    free_stream(s);
    return error;
}

//******************************************************************************
int
get_pixel_safe(char *data, int x, int y, int width, int height, int bpp)
{
    int start = 0;
    int shift = 0;

    if (x < 0)
    {
        return 0;
    }

    if (y < 0)
    {
        return 0;
    }

    if (x >= width)
    {
        return 0;
    }

    if (y >= height)
    {
        return 0;
    }

    if (bpp == 1)
    {
        width = (width + 7) / 8;
        start = (y * width) + x / 8;
        shift = x % 8;
        return (data[start] & (0x80 >> shift)) != 0;
    }
    else if (bpp == 4)
    {
        width = (width + 1) / 2;
        start = y * width + x / 2;
        shift = x % 2;

        if (shift == 0)
        {
            return (data[start] & 0xf0) >> 4;
        }
        else
        {
            return data[start] & 0x0f;
        }
    }
    else if (bpp == 8)
    {
        return *(((unsigned char *)data) + (y * width + x));
    }
    else if (bpp == 15 || bpp == 16)
    {
        return *(((unsigned short *)data) + (y * width + x));
    }
    else if (bpp == 24 || bpp == 32)
    {
        return *(((unsigned int *)data) + (y * width + x));
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "error in get_pixel_safe bpp %d", bpp);
    }

    return 0;
}

/******************************************************************************/
void
set_pixel_safe(char *data, int x, int y, int width, int height, int bpp,
               int pixel)
{
    int start = 0;
    int shift = 0;

    if (x < 0)
    {
        return;
    }

    if (y < 0)
    {
        return;
    }

    if (x >= width)
    {
        return;
    }

    if (y >= height)
    {
        return;
    }

    if (bpp == 1)
    {
        width = (width + 7) / 8;
        start = (y * width) + x / 8;
        shift = x % 8;

        if (pixel & 1)
        {
            data[start] = data[start] | (0x80 >> shift);
        }
        else
        {
            data[start] = data[start] & ~(0x80 >> shift);
        }
    }
    else if (bpp == 15 || bpp == 16)
    {
        *(((unsigned short *)data) + (y * width + x)) = pixel;
    }
    else if (bpp == 24)
    {
        *(data + (3 * (y * width + x)) + 0) = pixel >> 0;
        *(data + (3 * (y * width + x)) + 1) = pixel >> 8;
        *(data + (3 * (y * width + x)) + 2) = pixel >> 16;
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "error in set_pixel_safe bpp %d", bpp);
    }
}

/******************************************************************************/
int
split_color(int pixel, int *r, int *g, int *b, int bpp, int *palette)
{
    if (bpp == 8)
    {
        if (pixel >= 0 && pixel < 256 && palette != 0)
        {
            *r = (palette[pixel] >> 16) & 0xff;
            *g = (palette[pixel] >> 8) & 0xff;
            *b = (palette[pixel] >> 0) & 0xff;
        }
    }
    else if (bpp == 15)
    {
        *r = ((pixel >> 7) & 0xf8) | ((pixel >> 12) & 0x7);
        *g = ((pixel >> 2) & 0xf8) | ((pixel >> 8) & 0x7);
        *b = ((pixel << 3) & 0xf8) | ((pixel >> 2) & 0x7);
    }
    else if (bpp == 16)
    {
        *r = ((pixel >> 8) & 0xf8) | ((pixel >> 13) & 0x7);
        *g = ((pixel >> 3) & 0xfc) | ((pixel >> 9) & 0x3);
        *b = ((pixel << 3) & 0xf8) | ((pixel >> 2) & 0x7);
    }
    else if (bpp == 24 || bpp == 32)
    {
        *r = (pixel >> 16) & 0xff;
        *g = (pixel >> 8) & 0xff;
        *b = pixel & 0xff;
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "error in split_color bpp %d", bpp);
    }

    return 0;
}

/******************************************************************************/
int
make_color(int r, int g, int b, int bpp)
{
    if (bpp == 24)
    {
        return (r << 16) | (g << 8) | b;
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "error in make_color bpp %d", bpp);
    }

    return 0;
}

/**
 * Converts a bits-per-pixel value to bytes-per-pixel
 */
static int
get_bytes_per_pixel(int bpp)
{
    int result = (bpp + 7) / 8;

    if (result == 3)
    {
        result = 4;
    }

    return result;
}


/**************************************************************************//**
 * Skips the specified number of bytes from the transport
 *
 * @param transport Transport to read
 * @param bytes Bytes to skip
 * @return != 0 for error
 */
static int
skip_trans_bytes(struct trans *trans, int bytes)
{
    struct stream *s;
    int error;

    make_stream(s);
    init_stream(s, bytes);
    error = trans_force_read_s(trans, s, bytes);
    free_stream(s);

    return error;
}

/**************************************************************************//**
 * Reads an encoding from the input stream and discards it
 *
 * @param v VNC object
 * @param x Encoding X value
 * @param y Encoding Y value
 * @param cx Encoding CX value
 * @param cy Encoding CY value
 * @param encoding Code for encoding
 * @return != 0 for error
 *
 * @pre On entry the input stream is positioned after the encoding header
 */
static int
skip_encoding(struct vnc *v, int x, int y, int cx, int cy,
              encoding_type encoding)
{
    char text[256];
    int error = 0;

    switch (encoding)
    {
        case ENC_RAW:
        {
            int need_size = cx * cy * get_bytes_per_pixel(v->server_bpp);
            LOG(LOG_LEVEL_DEBUG, "Skipping ENC_RAW encoding");
            error = skip_trans_bytes(v->trans, need_size);
        }
        break;

        case ENC_COPY_RECT:
        {
            LOG(LOG_LEVEL_DEBUG, "Skipping ENC_COPY_RECT encoding");
            error = skip_trans_bytes(v->trans, 4);
        }
        break;

        case ENC_CURSOR:
        {
            int j = cx * cy * get_bytes_per_pixel(v->server_bpp);
            int k = ((cx + 7) / 8) * cy;

            LOG(LOG_LEVEL_DEBUG, "Skipping ENC_CURSOR encoding");
            error = skip_trans_bytes(v->trans, j + k);
        }
        break;

        case ENC_DESKTOP_SIZE:
            LOG(LOG_LEVEL_DEBUG, "Skipping ENC_DESKTOP_SIZE encoding");
            break;

        case ENC_EXTENDED_DESKTOP_SIZE:
        {
            struct vnc_screen_layout layout = {0};
            LOG(LOG_LEVEL_DEBUG,
                "Skipping ENC_EXTENDED_DESKTOP_SIZE encoding "
                "x=%d, y=%d geom=%dx%d",
                x, y, cx, cy);
            error = read_extended_desktop_size_rect(v, &layout);
            g_free(layout.s);
        }
        break;

        default:
            g_sprintf(text, "VNC error in skip_encoding "
                      "encoding = %8.8x", encoding);
            v->server_msg(v, text, 1);
    }

    return error;
}

/**************************************************************************//**
 * Parses an entire framebuffer update message from the wire, and returns the
 * first matching ExtendedDesktopSize encoding if found.
 *
 * Caller can check for a match by examining match_layout.s after the call
 *
 * @param v VNC object
 * @param match Function to call to check for a match
 * @param [out] match_x Matching x parameter for an encoding (if needed)
 * @param [out] match_y Matching y parameter for an encoding (if needed)
 * @param [out] match_layout Returned layout for the encoding
 * @return != 0 for error
 *
 * @post After a successful call, match_layout.s must be free'd
 */
static int
find_matching_extended_rect(struct vnc *v,
                            int (*match)(int x, int y, int cx, int cy),
                            int *match_x,
                            int *match_y,
                            struct vnc_screen_layout *match_layout)
{
    int error;
    struct stream *s;
    unsigned int num_rects;
    unsigned int i;
    int x;
    int y;
    int cx;
    int cy;
    encoding_type encoding;

    match_layout->s = NULL;

    make_stream(s);
    init_stream(s, 8192);
    error = trans_force_read_s(v->trans, s, 3);

    if (error == 0)
    {
        in_uint8s(s, 1);
        in_uint16_be(s, num_rects);

        for (i = 0; i < num_rects; ++i)
        {
            if (error != 0)
            {
                break;
            }

            init_stream(s, 8192);
            error = trans_force_read_s(v->trans, s, 12);

            if (error == 0)
            {
                in_uint16_be(s, x);
                in_uint16_be(s, y);
                in_uint16_be(s, cx);
                in_uint16_be(s, cy);
                in_uint32_be(s, encoding);

                if (encoding == ENC_EXTENDED_DESKTOP_SIZE &&
                        match_layout->s == NULL &&
                        match(x, y, cx, cy))
                {
                    LOG(LOG_LEVEL_DEBUG,
                        "VNC matched ExtendedDesktopSize rectangle "
                        "x=%d, y=%d geom=%dx%d",
                        x, y, cx, cy);

                    error = read_extended_desktop_size_rect(v, match_layout);
                    if (match_x)
                    {
                        *match_x = x;
                    }
                    if (match_y)
                    {
                        *match_y = y;
                    }
                    match_layout->total_width = cx;
                    match_layout->total_height = cy;
                }
                else
                {
                    error = skip_encoding(v, x, y, cx, cy, encoding);
                }
            }
        }
    }

    return error;
}

/**************************************************************************//**
 * Sends a FramebufferUpdateRequest for the resize status state machine
 *
 * The state machine is used at the start of the connection to negotiate
 * a common geometry between the client and the server.
 *
 * The RFB community wiki contains the following paragraph not present
 * in RFC6143:-
 *
 *     Note that an empty area can still solicit a FramebufferUpdate
 *     even though that update will only contain pseudo-encodings
 *
 * This doesn't seem to be as widely supported as we would like at
 * present. We will always request at least a single pixel update to
 * avoid confusing the server.
 *
 * @param v VNC object
 * @return != 0 for error
 */
static int
send_update_request_for_resize_status(struct vnc *v)
{
    int error = 0;
    struct stream *s;
    make_stream(s);
    init_stream(s, 8192);

    switch (v->resize_status)
    {
        case VRS_WAITING_FOR_FIRST_UPDATE:
            /*
             * Ask for an immediate, minimal update.
             */
            out_uint8(s, C2S_FRAMEBUFFER_UPDATE_REQUEST);
            out_uint8(s, 0); /* incremental == 0 : Full update */
            out_uint16_be(s, 0);
            out_uint16_be(s, 0);
            out_uint16_be(s, 1);
            out_uint16_be(s, 1);
            s_mark_end(s);
            error = lib_send_copy(v, s);
            break;

        case VRS_WAITING_FOR_RESIZE_CONFIRM:
            /*
             * Ask for a deferred minimal update.
             */
            out_uint8(s, C2S_FRAMEBUFFER_UPDATE_REQUEST);
            out_uint8(s, 1); /* incremental == 1 : Changes only */
            out_uint16_be(s, 0);
            out_uint16_be(s, 0);
            out_uint16_be(s, 1);
            out_uint16_be(s, 1);
            s_mark_end(s);
            error = lib_send_copy(v, s);
            break;

        default:
            /*
             * Ask for a full update from the server
             */
            if (v->suppress_output == 0)
            {
                out_uint8(s, C2S_FRAMEBUFFER_UPDATE_REQUEST);
                out_uint8(s, 0); /* incremental == 0 : Full update */
                out_uint16_be(s, 0);
                out_uint16_be(s, 0);
                out_uint16_be(s, v->server_width);
                out_uint16_be(s, v->server_height);
                s_mark_end(s);
                error = lib_send_copy(v, s);
            }
            break;
    }

    free_stream(s);

    return error;
}

/**************************************************************************//**
 * Tests if extended desktop size rect is an initial geometry specification
 *
 * This should be x == 0, but the specification says to treat undefined
 * values as 0 also */
static int
rect_is_initial_geometry(int x, int y, int cx, int cy)
{
    return (x != 1 && x != 2);
}

/**************************************************************************//**
 * Tests if extended desktop size rect is a reply to a request from us
 */
static int
rect_is_reply_to_us(int x, int y, int cx, int cy)
{
    return (x == 1);
}

/**************************************************************************//**
 * Returns an error string for an ExtendedDesktopSize status code
 */
static const char *
get_eds_status_msg(unsigned int response_code)
{
    if (response_code >= EDS_STATUS_MSG_COUNT)
    {
        response_code = EDS_STATUS_MSG_COUNT - 1;
    }

    return eds_status_msg[response_code];
}

/**************************************************************************//**
 * Handles the first framebuffer update from the server
 *
 * This is used to determine if the server supports resizes from
 * us. See The RFB community wiki for details.
 *
 * If the server does support resizing, we send our client geometry over.
 *
 * @param v VNC object
 * @return != 0 for error
 */
static int
lib_framebuffer_first_update(struct vnc *v)
{
    int error;
    struct vnc_screen_layout layout = {0};

    error = find_matching_extended_rect(v,
                                        rect_is_initial_geometry,
                                        NULL,
                                        NULL,
                                        &layout);
    if (error == 0)
    {
        if (layout.s != NULL)
        {
            LOG(LOG_LEVEL_DEBUG, "VNC server supports resizing");

            /* Force the client geometry over to the server */
            log_screen_layout(LOG_LEVEL_INFO, "OldLayout", &layout);

            /*
             * If we've only got one screen, and the other side has
             * only got one screen, we will preserve their screen ID
             * and any flags.  This may prevent us sending an unwanted
             * SetDesktopSize message if the screen dimensions are
             * a match. We can't do this with more than one screen,
             * as we have no way to map different IDs
             */
            if (layout.count == 1 && v->client_layout.count == 1)
            {
                LOG(LOG_LEVEL_DEBUG, "VNC "
                    "setting screen id to %d from server",
                    layout.s[0].id);

                v->client_layout.s[0].id = layout.s[0].id;
                v->client_layout.s[0].flags = layout.s[0].flags;
            }

            if (vnc_screen_layouts_equal(&layout, &v->client_layout))
            {
                LOG(LOG_LEVEL_DEBUG, "Server layout is the same "
                    "as the client layout");
                v->resize_status = VRS_DONE;
            }
            else
            {
                LOG(LOG_LEVEL_DEBUG, "Server layout differs from "
                    "the client layout. Changing server layout");
                error = send_set_desktop_size(v, &v->client_layout);
                v->resize_status = VRS_WAITING_FOR_RESIZE_CONFIRM;
            }
        }
        else
        {
            LOG(LOG_LEVEL_DEBUG, "VNC server does not support resizing");

            /* Force client to same size as server */
            LOG(LOG_LEVEL_DEBUG, "Resizing client to server %dx%d",
                v->server_width, v->server_height);
            error = resize_client(v, 0, v->server_width, v->server_height);
            v->resize_status = VRS_DONE;
        }

        g_free(layout.s);
    }

    if (error == 0)
    {
        error = send_update_request_for_resize_status(v);
    }

    return error;
}

/**************************************************************************//**
 * Looks for a resize confirm in a framebuffer update request
 *
 * If the server supports resizes from us, this is used to find the
 * reply to our initial resize request. See The RFB community wiki for details.
 *
 * @param v VNC object
 * @return != 0 for error
 */
static int
lib_framebuffer_waiting_for_resize_confirm(struct vnc *v)
{
    int error;
    struct vnc_screen_layout layout = {0};
    int response_code;

    error = find_matching_extended_rect(v,
                                        rect_is_reply_to_us,
                                        NULL,
                                        &response_code,
                                        &layout);
    if (error == 0)
    {
        if (layout.s != NULL)
        {
            if (response_code == 0)
            {
                LOG(LOG_LEVEL_DEBUG, "VNC server successfully resized");
                log_screen_layout(LOG_LEVEL_INFO, "NewLayout", &layout);
            }
            else
            {
                LOG(LOG_LEVEL_WARNING,
                    "VNC server resize failed - error code %d [%s]",
                    response_code,
                    get_eds_status_msg(response_code));
                /* Force client to same size as server */
                LOG(LOG_LEVEL_WARNING, "Resizing client to server %dx%d",
                    v->server_width, v->server_height);
                error = resize_client(v, 0, v->server_width, v->server_height);
            }
            v->resize_status = VRS_DONE;
        }

        g_free(layout.s);
    }

    if (error == 0)
    {
        error = send_update_request_for_resize_status(v);
    }

    return error;
}

/******************************************************************************/
int
lib_framebuffer_update(struct vnc *v)
{
    char *d1;
    char *d2;
    char cursor_data[32 * (32 * 3)];
    char cursor_mask[32 * (32 / 8)];
    char text[256];
    int num_recs;
    int i;
    int j;
    int k;
    int x;
    int y;
    int cx;
    int cy;
    int srcx;
    int srcy;
    unsigned int encoding;
    int pixel;
    int r;
    int g;
    int b;
    int error;
    int need_size;
    struct stream *s;
    struct stream *pixel_s;
    struct vnc_screen_layout layout = { 0 };

    num_recs = 0;

    make_stream(pixel_s);

    make_stream(s);
    init_stream(s, 8192);
    error = trans_force_read_s(v->trans, s, 3);

    if (error == 0)
    {
        in_uint8s(s, 1);
        in_uint16_be(s, num_recs);
        error = v->server_begin_update(v);
    }

    for (i = 0; i < num_recs; i++)
    {
        if (error != 0)
        {
            break;
        }

        init_stream(s, 8192);
        error = trans_force_read_s(v->trans, s, 12);

        if (error == 0)
        {
            in_uint16_be(s, x);
            in_uint16_be(s, y);
            in_uint16_be(s, cx);
            in_uint16_be(s, cy);
            in_uint32_be(s, encoding);

            if (encoding == ENC_RAW)
            {
                need_size = cx * cy * get_bytes_per_pixel(v->server_bpp);
                init_stream(pixel_s, need_size);
                error = trans_force_read_s(v->trans, pixel_s, need_size);

                if (error == 0)
                {
                    error = v->server_paint_rect(v, x, y, cx, cy, pixel_s->data, cx, cy, 0, 0);
                }
            }
            else if (encoding == ENC_COPY_RECT)
            {
                init_stream(s, 8192);
                error = trans_force_read_s(v->trans, s, 4);

                if (error == 0)
                {
                    in_uint16_be(s, srcx);
                    in_uint16_be(s, srcy);
                    error = v->server_screen_blt(v, x, y, cx, cy, srcx, srcy);
                }
            }
            else if (encoding == ENC_CURSOR)
            {
                g_memset(cursor_data, 0, 32 * (32 * 3));
                g_memset(cursor_mask, 0, 32 * (32 / 8));
                j = cx * cy * get_bytes_per_pixel(v->server_bpp);
                k = ((cx + 7) / 8) * cy;
                init_stream(s, j + k);
                error = trans_force_read_s(v->trans, s, j + k);

                if (error == 0)
                {
                    in_uint8p(s, d1, j);
                    in_uint8p(s, d2, k);

                    for (j = 0; j < 32; j++)
                    {
                        for (k = 0; k < 32; k++)
                        {
                            pixel = get_pixel_safe(d2, k, 31 - j, cx, cy, 1);
                            set_pixel_safe(cursor_mask, k, j, 32, 32, 1, !pixel);

                            if (pixel)
                            {
                                pixel = get_pixel_safe(d1, k, 31 - j, cx, cy, v->server_bpp);
                                split_color(pixel, &r, &g, &b, v->server_bpp, v->palette);
                                pixel = make_color(r, g, b, 24);
                                set_pixel_safe(cursor_data, k, j, 32, 32, 24, pixel);
                            }
                        }
                    }

                    /* keep these in 32x32, vnc cursor can be a lot bigger */
                    if (x > 31)
                    {
                        x = 31;
                    }

                    if (y > 31)
                    {
                        y = 31;
                    }

                    error = v->server_set_cursor(v, x, y, cursor_data, cursor_mask);
                }
            }
            else if (encoding == ENC_DESKTOP_SIZE)
            {
                /* Server end has resized */
                v->server_width = cx;
                v->server_height = cy;
                error = resize_client(v, 1, cx, cy);
            }
            else if (encoding == ENC_EXTENDED_DESKTOP_SIZE)
            {
                layout.total_width = cx;
                layout.total_height = cy;
                error = read_extended_desktop_size_rect(v, &layout);
                /* If this is a reply to a request from us, x == 1 */
                if (error == 0 && x != 1)
                {
                    v->server_width = layout.total_width;
                    v->server_height = layout.total_height;
                    error = resize_client_from_layout(v, 1, &layout);
                }
                g_free(layout.s);
            }
            else
            {
                g_sprintf(text, "VNC error in lib_framebuffer_update encoding = %8.8x",
                          encoding);
                v->server_msg(v, text, 1);
            }
        }
    }

    if (error == 0)
    {
        error = v->server_end_update(v);
    }

    if (error == 0)
    {
        if (v->suppress_output == 0)
        {
            init_stream(s, 8192);
            out_uint8(s, C2S_FRAMEBUFFER_UPDATE_REQUEST);
            out_uint8(s, 1); /* incremental == 1 : Changes only */
            out_uint16_be(s, 0);
            out_uint16_be(s, 0);
            out_uint16_be(s, v->server_width);
            out_uint16_be(s, v->server_height);
            s_mark_end(s);
            error = lib_send_copy(v, s);
        }
    }

    free_stream(s);
    free_stream(pixel_s);
    return error;
}

/******************************************************************************/
/* clip data from the vnc server */
int
lib_clip_data(struct vnc *v)
{
    struct stream *s;
    struct stream *out_s;
    int size;
    int error;

    free_stream(v->clip_data_s);
    v->clip_data_s = 0;
    make_stream(s);
    init_stream(s, 8192);
    error = trans_force_read_s(v->trans, s, 7);

    if (error == 0)
    {
        in_uint8s(s, 3);
        in_uint32_be(s, size);
        make_stream(v->clip_data_s);
        init_stream(v->clip_data_s, size);
        error = trans_force_read_s(v->trans, v->clip_data_s, size);
    }

    if (error == 0)
    {
        make_stream(out_s);
        init_stream(out_s, 8192);
        out_uint16_le(out_s, 2);
        out_uint16_le(out_s, 0);
        out_uint32_le(out_s, 0x90);
        out_uint8(out_s, 0x0d);
        out_uint8s(out_s, 35);
        out_uint8(out_s, 0x10);
        out_uint8s(out_s, 35);
        out_uint8(out_s, 0x01);
        out_uint8s(out_s, 35);
        out_uint8(out_s, 0x07);
        out_uint8s(out_s, 35);
        out_uint8s(out_s, 4);
        s_mark_end(out_s);
        size = (int)(out_s->end - out_s->data);
        error = v->server_send_to_channel(v, v->clip_chanid, out_s->data, size, size, 3);
        free_stream(out_s);
    }

    free_stream(s);
    return error;
}

/******************************************************************************/
int
lib_palette_update(struct vnc *v)
{
    struct stream *s;
    int first_color;
    int num_colors;
    int i;
    int r;
    int g;
    int b;
    int error;

    make_stream(s);
    init_stream(s, 8192);
    error = trans_force_read_s(v->trans, s, 5);

    if (error == 0)
    {
        in_uint8s(s, 1);
        in_uint16_be(s, first_color);
        in_uint16_be(s, num_colors);
        init_stream(s, 8192);
        error = trans_force_read_s(v->trans, s, num_colors * 6);
    }

    if (error == 0)
    {
        for (i = 0; i < num_colors; i++)
        {
            in_uint16_be(s, r);
            in_uint16_be(s, g);
            in_uint16_be(s, b);
            r = r >> 8;
            g = g >> 8;
            b = b >> 8;
            v->palette[first_color + i] = (r << 16) | (g << 8) | b;
        }

        error = v->server_begin_update(v);
    }

    if (error == 0)
    {
        error = v->server_palette(v, v->palette);
    }

    if (error == 0)
    {
        error = v->server_end_update(v);
    }

    free_stream(s);
    return error;
}

/******************************************************************************/
int
lib_bell_trigger(struct vnc *v)
{
    int error;

    error = v->server_bell_trigger(v);
    return error;
}

/******************************************************************************/
int
lib_mod_signal(struct vnc *v)
{
    return 0;
}

/******************************************************************************/
static int
lib_mod_process_message(struct vnc *v, struct stream *s)
{
    char type;
    int error;
    char text[256];

    in_uint8(s, type);

    error = 0;
    if (error == 0)
    {
        if (type == S2C_FRAMEBUFFER_UPDATE)
        {
            switch (v->resize_status)
            {
                case VRS_WAITING_FOR_FIRST_UPDATE:
                    error = lib_framebuffer_first_update(v);
                    break;

                case VRS_WAITING_FOR_RESIZE_CONFIRM:
                    error = lib_framebuffer_waiting_for_resize_confirm(v);
                    break;

                default:
                    error = lib_framebuffer_update(v);
            }
        }
        else if (type == S2C_SET_COLOUR_MAP_ENTRIES)
        {
            error = lib_palette_update(v);
        }
        else if (type == S2C_BELL)
        {
            error = lib_bell_trigger(v);
        }
        else if (type == S2C_SERVER_CUT_TEXT) /* clipboard */
        {
            LOG(LOG_LEVEL_DEBUG, "VNC got clip data");
            error = lib_clip_data(v);
        }
        else
        {
            g_sprintf(text, "VNC unknown in lib_mod_process_message %d", type);
            v->server_msg(v, text, 1);
        }
    }

    return error;
}

/******************************************************************************/
int
lib_mod_start(struct vnc *v, int w, int h, int bpp)
{
    v->server_begin_update(v);
    v->server_set_fgcolor(v, 0);
    v->server_fill_rect(v, 0, 0, w, h);
    v->server_end_update(v);
    v->server_bpp = bpp;
    return 0;
}

/******************************************************************************/
static int
lib_open_clip_channel(struct vnc *v)
{
    char init_data[12] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    v->clip_chanid = v->server_get_channel_id(v, "cliprdr");

    if (v->clip_chanid >= 0)
    {
        v->server_send_to_channel(v, v->clip_chanid, init_data, 12, 12, 3);
    }

    return 0;
}

/******************************************************************************/
static int
lib_data_in(struct trans *trans)
{
    struct vnc *self;
    struct stream *s;

    LOG_DEVEL(LOG_LEVEL_TRACE, "lib_data_in:");

    if (trans == 0)
    {
        return 1;
    }

    self = (struct vnc *)(trans->callback_data);
    s = trans_get_in_s(trans);

    if (s == 0)
    {
        return 1;
    }

    if (lib_mod_process_message(self, s) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "lib_data_in: lib_mod_process_message failed");
        return 1;
    }

    init_stream(s, 0);

    return 0;
}

/******************************************************************************/
/*
  return error
*/
int
lib_mod_connect(struct vnc *v)
{
    char cursor_data[32 * (32 * 3)];
    char cursor_mask[32 * (32 / 8)];
    char con_port[256];
    char text[256];
    struct stream *s;
    struct stream *pixel_format;
    int error;
    int i;
    int check_sec_result;

    v->server_msg(v, "VNC started connecting", 0);
    check_sec_result = 1;

    /* check if bpp is supported for rdp connection */
    switch (v->server_bpp)
    {
        case 8:
        case 15:
        case 16:
        case 24:
        case 32:
            break;
        default:
            v->server_msg(v, "VNC error - only supporting 8, 15, 16, 24 and 32 "
                          "bpp rdp connections", 0);
            return 1;
    }

    if (g_strcmp(v->ip, "") == 0)
    {
        v->server_msg(v, "VNC error - no ip set", 0);
        return 1;
    }

    make_stream(s);
    g_sprintf(con_port, "%s", v->port);
    make_stream(pixel_format);

    v->trans = trans_create(TRANS_MODE_TCP, 8 * 8192, 8192);
    if (v->trans == 0)
    {
        v->server_msg(v, "VNC error: trans_create() failed", 0);
        free_stream(s);
        free_stream(pixel_format);
        return 1;
    }

    v->sck_closed = 0;
    if (v->delay_ms > 0)
    {
        g_sprintf(text, "Waiting %d ms for VNC to start...", v->delay_ms);
        v->server_msg(v, text, 0);
        g_sleep(v->delay_ms);
    }

    g_sprintf(text, "VNC connecting to %s %s", v->ip, con_port);
    v->server_msg(v, text, 0);

    v->trans->si = v->si;
    v->trans->my_source = XRDP_SOURCE_MOD;

    error = trans_connect(v->trans, v->ip, con_port, 3000);

    if (error == 0)
    {
        v->server_msg(v, "VNC tcp connected", 0);
        /* protocol version */
        init_stream(s, 8192);
        error = trans_force_read_s(v->trans, s, 12);
        if (error == 0)
        {
            s->p = s->data;
            out_uint8a(s, "RFB 003.003\n", 12);
            s_mark_end(s);
            error = trans_force_write_s(v->trans, s);
        }

        /* sec type */
        if (error == 0)
        {
            init_stream(s, 8192);
            error = trans_force_read_s(v->trans, s, 4);
        }

        if (error == 0)
        {
            in_uint32_be(s, i);
            g_sprintf(text, "VNC security level is %d (1 = none, 2 = standard)", i);
            v->server_msg(v, text, 0);

            if (i == 1) /* none */
            {
                check_sec_result = 0;
            }
            else if (i == 2) /* dec the password and the server random */
            {
                init_stream(s, 8192);
                error = trans_force_read_s(v->trans, s, 16);

                if (error == 0)
                {
                    init_stream(s, 8192);
                    if (v->got_guid)
                    {
                        char guid_str[64];
                        g_bytes_to_hexstr(v->guid, 16, guid_str, 64);
                        rfbHashEncryptBytes(s->data, guid_str);
                    }
                    else
                    {
                        rfbEncryptBytes(s->data, v->password);
                    }
                    s->p += 16;
                    s_mark_end(s);
                    error = trans_force_write_s(v->trans, s);
                    check_sec_result = 1; // not needed
                }
            }
            else if (i == 0)
            {
                LOG(LOG_LEVEL_ERROR, "VNC Server will disconnect");
                error = 1;
            }
            else
            {
                LOG(LOG_LEVEL_ERROR, "VNC unsupported security level %d", i);
                error = 1;
            }
        }
    }

    if (error != 0)
    {
        LOG(LOG_LEVEL_ERROR, "VNC error %d after security negotiation",
            error);
    }

    if (error == 0 && check_sec_result)
    {
        /* sec result */
        init_stream(s, 8192);
        error = trans_force_read_s(v->trans, s, 4);

        if (error == 0)
        {
            in_uint32_be(s, i);

            if (i != 0)
            {
                v->server_msg(v, "VNC password failed", 0);
                error = 2;
            }
            else
            {
                v->server_msg(v, "VNC password ok", 0);
            }
        }
    }

    if (error == 0)
    {
        v->server_msg(v, "VNC sending share flag", 0);
        init_stream(s, 8192);
        s->data[0] = 1;
        s->p++;
        s_mark_end(s);
        error = trans_force_write_s(v->trans, s); /* share flag */
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "VNC error before sending share flag");
    }

    if (error == 0)
    {
        v->server_msg(v, "VNC receiving server init", 0);
        init_stream(s, 8192);
        error = trans_force_read_s(v->trans, s, 4); /* server init */
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "VNC error before receiving server init");
    }

    if (error == 0)
    {
        in_uint16_be(s, v->server_width);
        in_uint16_be(s, v->server_height);

        init_stream(pixel_format, 8192);
        v->server_msg(v, "VNC receiving pixel format", 0);
        error = trans_force_read_s(v->trans, pixel_format, 16);
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "VNC error before receiving pixel format");
    }

    if (error == 0)
    {
        init_stream(s, 8192);
        v->server_msg(v, "VNC receiving name length", 0);
        error = trans_force_read_s(v->trans, s, 4); /* name len */
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "VNC error before receiving name length");
    }

    if (error == 0)
    {
        in_uint32_be(s, i);

        if (i > 255 || i < 0)
        {
            error = 3;
        }
        else
        {
            init_stream(s, 8192);
            v->server_msg(v, "VNC receiving name", 0);
            error = trans_force_read_s(v->trans, s, i); /* name len */
            g_memcpy(v->mod_name, s->data, i);
            v->mod_name[i] = 0;
        }
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "VNC error before receiving name");
    }

    /* should be connected */
    if (error == 0)
    {
        init_stream(s, 8192);
        out_uint8(s, C2S_SET_PIXEL_FORMAT);
        out_uint8(s, 0);
        out_uint8(s, 0);
        out_uint8(s, 0);
        init_stream(pixel_format, 8192);

        if (v->server_bpp == 8)
        {
            out_uint8(pixel_format, 8); /* bits per pixel */
            out_uint8(pixel_format, 8); /* depth */
#if defined(B_ENDIAN)
            out_uint8(pixel_format, 1); /* big endian */
#else
            out_uint8(pixel_format, 0); /* big endian */
#endif
            out_uint8(pixel_format, 0); /* true color flag */
            out_uint16_be(pixel_format, 0); /* red max */
            out_uint16_be(pixel_format, 0); /* green max */
            out_uint16_be(pixel_format, 0); /* blue max */
            out_uint8(pixel_format, 0); /* red shift */
            out_uint8(pixel_format, 0); /* green shift */
            out_uint8(pixel_format, 0); /* blue shift */
            out_uint8s(pixel_format, 3); /* pad */
        }
        else if (v->server_bpp == 15)
        {
            out_uint8(pixel_format, 16); /* bits per pixel */
            out_uint8(pixel_format, 15); /* depth */
#if defined(B_ENDIAN)
            out_uint8(pixel_format, 1); /* big endian */
#else
            out_uint8(pixel_format, 0); /* big endian */
#endif
            out_uint8(pixel_format, 1); /* true color flag */
            out_uint16_be(pixel_format, 31); /* red max */
            out_uint16_be(pixel_format, 31); /* green max */
            out_uint16_be(pixel_format, 31); /* blue max */
            out_uint8(pixel_format, 10); /* red shift */
            out_uint8(pixel_format, 5); /* green shift */
            out_uint8(pixel_format, 0); /* blue shift */
            out_uint8s(pixel_format, 3); /* pad */
        }
        else if (v->server_bpp == 16)
        {
            out_uint8(pixel_format, 16); /* bits per pixel */
            out_uint8(pixel_format, 16); /* depth */
#if defined(B_ENDIAN)
            out_uint8(pixel_format, 1); /* big endian */
#else
            out_uint8(pixel_format, 0); /* big endian */
#endif
            out_uint8(pixel_format, 1); /* true color flag */
            out_uint16_be(pixel_format, 31); /* red max */
            out_uint16_be(pixel_format, 63); /* green max */
            out_uint16_be(pixel_format, 31); /* blue max */
            out_uint8(pixel_format, 11); /* red shift */
            out_uint8(pixel_format, 5); /* green shift */
            out_uint8(pixel_format, 0); /* blue shift */
            out_uint8s(pixel_format, 3); /* pad */
        }
        else if (v->server_bpp == 24 || v->server_bpp == 32)
        {
            out_uint8(pixel_format, 32); /* bits per pixel */
            out_uint8(pixel_format, 24); /* depth */
#if defined(B_ENDIAN)
            out_uint8(pixel_format, 1); /* big endian */
#else
            out_uint8(pixel_format, 0); /* big endian */
#endif
            out_uint8(pixel_format, 1); /* true color flag */
            out_uint16_be(pixel_format, 255); /* red max */
            out_uint16_be(pixel_format, 255); /* green max */
            out_uint16_be(pixel_format, 255); /* blue max */
            out_uint8(pixel_format, 16); /* red shift */
            out_uint8(pixel_format, 8); /* green shift */
            out_uint8(pixel_format, 0); /* blue shift */
            out_uint8s(pixel_format, 3); /* pad */
        }

        out_uint8a(s, pixel_format->data, 16);
        v->server_msg(v, "VNC sending pixel format", 0);
        s_mark_end(s);
        error = trans_force_write_s(v->trans, s);
    }

    if (error == 0)
    {
        encoding_type e[10];
        unsigned int n = 0;
        unsigned int i;

        /* These encodings are always supported */
        e[n++] = ENC_RAW;
        e[n++] = ENC_COPY_RECT;
        e[n++] = ENC_CURSOR;
        e[n++] = ENC_DESKTOP_SIZE;
        if (v->enabled_encodings_mask & MSK_EXTENDED_DESKTOP_SIZE)
        {
            e[n++] = ENC_EXTENDED_DESKTOP_SIZE;
        }
        else
        {
            LOG(LOG_LEVEL_INFO,
                "VNC User disabled EXTENDED_DESKTOP_SIZE");
        }

        init_stream(s, 8192);
        out_uint8(s, C2S_SET_ENCODINGS);
        out_uint8(s, 0);
        out_uint16_be(s, n); /* Number of encodings following */
        for (i = 0 ; i < n; ++i)
        {
            out_uint32_be(s, e[i]);
        }
        s_mark_end(s);
        error = trans_force_write_s(v->trans, s);
    }

    if (error == 0)
    {
        v->resize_status = VRS_WAITING_FOR_FIRST_UPDATE;
        error = send_update_request_for_resize_status(v);
    }

    if (error == 0)
    {
        /* set almost null cursor, this is the little dot cursor */
        g_memset(cursor_data, 0, 32 * (32 * 3));
        g_memset(cursor_data + (32 * (32 * 3) - 1 * 32 * 3), 0xff, 9);
        g_memset(cursor_data + (32 * (32 * 3) - 2 * 32 * 3), 0xff, 9);
        g_memset(cursor_data + (32 * (32 * 3) - 3 * 32 * 3), 0xff, 9);
        g_memset(cursor_mask, 0xff, 32 * (32 / 8));
        v->server_msg(v, "VNC sending cursor", 0);
        error = v->server_set_cursor(v, 3, 3, cursor_data, cursor_mask);
    }

    free_stream(s);
    free_stream(pixel_format);

    if (error == 0)
    {
        v->server_msg(v, "VNC connection complete, connected ok", 0);
        lib_open_clip_channel(v);
    }
    else
    {
        v->server_msg(v, "VNC error - problem connecting", 0);
    }

    if (error != 0)
    {
        trans_delete(v->trans);
        v->trans = 0;
        v->server_msg(v, "some problem", 0);
        return 1;
    }
    else
    {
        v->server_msg(v, "connected ok", 0);
        v->trans->trans_data_in = lib_data_in;
        v->trans->header_size = 1;
        v->trans->callback_data = v;
    }

    return error;
}

/******************************************************************************/
int
lib_mod_end(struct vnc *v)
{
    if (v->vnc_desktop != 0)
    {
    }

    free_stream(v->clip_data_s);
    return 0;
}

/**************************************************************************//**
 * Initialises the client layout from the Windows monitor definition.
 *
 * @param [out] layout Our layout
 * @param [in] client_info WM info
 */
static void
init_client_layout(struct vnc_screen_layout *layout,
                   const struct xrdp_client_info *client_info)
{
    int i;

    layout->total_width = client_info->width;
    layout->total_height = client_info->height;

    layout->count = client_info->monitorCount;
    layout->s = g_new(struct vnc_screen, layout->count);

    for (i = 0 ; i < client_info->monitorCount ; ++i)
    {
        /* Use minfo_wm, as this is normalised for a top-left of (0,0)
         * as required by RFC6143 */
        layout->s[i].id = i;
        layout->s[i].x = client_info->minfo_wm[i].left;
        layout->s[i].y = client_info->minfo_wm[i].top;
        layout->s[i].width =  client_info->minfo_wm[i].right -
                              client_info->minfo_wm[i].left + 1;
        layout->s[i].height = client_info->minfo_wm[i].bottom -
                              client_info->minfo_wm[i].top + 1;
        layout->s[i].flags = 0;
    }
}

/******************************************************************************/
int
lib_mod_set_param(struct vnc *v, const char *name, const char *value)
{
    if (g_strcasecmp(name, "username") == 0)
    {
        g_strncpy(v->username, value, 255);
    }
    else if (g_strcasecmp(name, "password") == 0)
    {
        g_strncpy(v->password, value, 255);
    }
    else if (g_strcasecmp(name, "ip") == 0)
    {
        g_strncpy(v->ip, value, 255);
    }
    else if (g_strcasecmp(name, "port") == 0)
    {
        g_strncpy(v->port, value, 255);
    }
    else if (g_strcasecmp(name, "keylayout") == 0)
    {
        v->keylayout = g_atoi(value);
    }
    else if (g_strcasecmp(name, "delay_ms") == 0)
    {
        v->delay_ms = g_atoi(value);
    }
    else if (g_strcasecmp(name, "guid") == 0)
    {
        v->got_guid = 1;
        g_memcpy(v->guid, value, 16);
    }
    else if (g_strcasecmp(name, "disabled_encodings_mask") == 0)
    {
        v->enabled_encodings_mask = ~g_atoi(value);
    }
    else if (g_strcasecmp(name, "client_info") == 0)
    {
        const struct xrdp_client_info *client_info =
            (const struct xrdp_client_info *) value;

        g_free(v->client_layout.s);

        /* Save monitor information from the client */
        if (!client_info->multimon || client_info->monitorCount < 1)
        {
            set_single_screen_layout(&v->client_layout,
                                     client_info->width,
                                     client_info->height);
        }
        else
        {
            init_client_layout(&v->client_layout, client_info);
        }
        log_screen_layout(LOG_LEVEL_DEBUG, "client_info", &v->client_layout);
    }


    return 0;
}

/******************************************************************************/
/* return error */
int
lib_mod_get_wait_objs(struct vnc *v, tbus *read_objs, int *rcount,
                      tbus *write_objs, int *wcount, int *timeout)
{
    LOG_DEVEL(LOG_LEVEL_TRACE, "lib_mod_get_wait_objs:");

    if (v != 0)
    {
        if (v->trans != 0)
        {
            trans_get_wait_objs_rw(v->trans, read_objs, rcount,
                                   write_objs, wcount, timeout);
        }
    }

    return 0;
}

/******************************************************************************/
/* return error */
int
lib_mod_check_wait_objs(struct vnc *v)
{
    int rv;

    rv = 0;
    if (v != 0)
    {
        if (v->trans != 0)
        {
            rv = trans_check_wait_objs(v->trans);
        }
    }
    return rv;
}

/******************************************************************************/
/* return error */
int
lib_mod_frame_ack(struct vnc *v, int flags, int frame_id)
{
    return 0;
}

/******************************************************************************/
/* return error */
int
lib_mod_suppress_output(struct vnc *v, int suppress,
                        int left, int top, int right, int bottom)
{
    int error;
    struct stream *s;

    error = 0;
    v->suppress_output = suppress;
    if (suppress == 0)
    {
        make_stream(s);
        init_stream(s, 8192);
        out_uint8(s, C2S_FRAMEBUFFER_UPDATE_REQUEST);
        out_uint8(s, 0); /* incremental == 0 : Full contents */
        out_uint16_be(s, 0);
        out_uint16_be(s, 0);
        out_uint16_be(s, v->server_width);
        out_uint16_be(s, v->server_height);
        s_mark_end(s);
        error = lib_send_copy(v, s);
        free_stream(s);
    }
    return error;
}

/******************************************************************************/
/* return error */
int
lib_mod_server_version_message(struct vnc *v)
{
    return 0;
}

/******************************************************************************/
/* return error */
int
lib_mod_server_monitor_resize(struct vnc *v, int width, int height)
{
    int error = 0;
    set_single_screen_layout(&v->client_layout, width, height);
    v->resize_status = VRS_WAITING_FOR_FIRST_UPDATE;
    error = send_update_request_for_resize_status(v);
    return error;
}

/******************************************************************************/
/* return error */
int
lib_mod_server_monitor_full_invalidate(struct vnc *v, int param1, int param2)
{
    return 0;
}

static struct x11vnckey kk(int attrs, int sym, int shiftedSym)
{
  struct x11vnckey result;
  result.attrs=(X11VncKeyAttrs)attrs;
  result.vncKeyCode=sym;
  result.shiftedVncKeyCode=shiftedSym;
  return result;
}
/******************************************************************************/
tintptr EXPORT_CC
mod_init(void)
{
    struct x11vnc *v;
    struct x11vnckey* keys;

    v = (struct x11vnc *)g_malloc(sizeof(struct x11vnc), 1);
    /* set client functions */
    v->_.size = sizeof(struct x11vnc);
    v->_.version = CURRENT_MOD_VER;
    v->_.handle = (tintptr) v;
    v->_.mod_connect = lib_mod_connect;
    v->_.mod_start = lib_mod_start;
    v->_.mod_event = lib_mod_event;
    v->_.mod_signal = lib_mod_signal;
    v->_.mod_end = lib_mod_end;
    v->_.mod_set_param = lib_mod_set_param;
    v->_.mod_get_wait_objs = lib_mod_get_wait_objs;
    v->_.mod_check_wait_objs = lib_mod_check_wait_objs;
    v->_.mod_frame_ack = lib_mod_frame_ack;
    v->_.mod_suppress_output = lib_mod_suppress_output;
    v->_.mod_server_monitor_resize = lib_mod_server_monitor_resize;
    v->_.mod_server_monitor_full_invalidate = lib_mod_server_monitor_full_invalidate;
    v->_.mod_server_version_message = lib_mod_server_version_message;
    v->_.mod_handle_key = lib_mod_handle_key;

    /* Member variables */
    v->_.enabled_encodings_mask = -1;
    bzero(&v->keys[0], sizeof(v->keys)*sizeof(v->keys[0]));
    keys=&v->keys[0];

    v->capsLocked = 0;
    v->numLocked = 0;
    
    const int AUTOREPEAT=X11_VNC_KEY_AUTO_REPEAT;
    const int CAPSLOCKABLE=X11_VNC_KEY_CAPS_LOCKABLE;
    //int NUMLOCKABLE=X11_VNC_KEY_NUM_LOCKABLE;
    const int IS_CAPSLOCK=X11_VNC_KEY_IS_CAPSLOCK;
    const int IS_NUMLOCK=X11_VNC_KEY_IS_NUMLOCK;
    
    //a-z
    keys[30] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0061, 0x0041);
    keys[48] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0062, 0x0042);
    keys[46] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0063, 0x0043);
    keys[32] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0064, 0x0044);
    keys[18] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0065, 0x0045);
    keys[33] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0066, 0x0046);
    keys[34] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0067, 0x0047);
    keys[35] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0068, 0x0048);
    keys[23] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0069, 0x0049);
    keys[36] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x006a, 0x004a);
    keys[37] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x006b, 0x004b);
    keys[38] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x006c, 0x004c);
    keys[50] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x006d, 0x004d);
    keys[49] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x006e, 0x004e);
    keys[24] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x006f, 0x004f);
    keys[25] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0070, 0x0050);
    keys[16] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0071, 0x0051);
    keys[19] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0072, 0x0052);
    keys[31] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0073, 0x0053);
    keys[20] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0074, 0x0054);
    keys[22] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0075, 0x0055);
    keys[47] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0076, 0x0056);
    keys[17] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0077, 0x0057);
    keys[45] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0078, 0x0058);
    keys[21] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x0079, 0x0059);
    keys[44] = kk(AUTOREPEAT|CAPSLOCKABLE, 0x007a, 0x005a);

    // 0-9
    keys[11] = kk(AUTOREPEAT, 0x0030, 0x0029);
    keys[ 2] = kk(AUTOREPEAT, 0x0031, 0x0021);
    keys[ 3] = kk(AUTOREPEAT, 0x0032, 0x0040);
    keys[ 4] = kk(AUTOREPEAT, 0x0033, 0x0023);
    keys[ 5] = kk(AUTOREPEAT, 0x0034, 0x0024);
    keys[ 6] = kk(AUTOREPEAT, 0x0035, 0x0025);
    keys[ 7] = kk(AUTOREPEAT, 0x0036, 0x005e);
    keys[ 8] = kk(AUTOREPEAT, 0x0037, 0x0026);
    keys[ 9] = kk(AUTOREPEAT, 0x0038, 0x002a);
    keys[10] = kk(AUTOREPEAT, 0x0039, 0x0028);
    // F1-F12
    keys[59] = kk(AUTOREPEAT, 0xffbe, 0xffbe);
    keys[60] = kk(AUTOREPEAT, 0xffbf, 0xffbf);
    keys[61] = kk(AUTOREPEAT, 0xffc0, 0xffc0);
    keys[62] = kk(AUTOREPEAT, 0xffc1, 0xffc1);
    keys[63] = kk(AUTOREPEAT, 0xffc2, 0xffc2);
    keys[64] = kk(AUTOREPEAT, 0xffc3, 0xffc3);
    keys[65] = kk(AUTOREPEAT, 0xffc4, 0xffc4);
    keys[66] = kk(AUTOREPEAT, 0xffc5, 0xffc5);
    keys[67] = kk(AUTOREPEAT, 0xffc6, 0xffc6);
    keys[68] = kk(AUTOREPEAT, 0xffc7, 0xffc7);
    keys[87] = kk(AUTOREPEAT, 0xffc8, 0xffc8);
    keys[88] = kk(AUTOREPEAT, 0xffc9, 0xffc9);
    // mods
    // shift, ctrl, alt
    keys[42] = kk(0, 0xffe1, 0xffe1);
    keys[29] = kk(0, 0xffe3, 0xffe3);
    keys[56] = kk(0, 0xffe9, 0xffe9);

    // capslock
    keys[58] = kk(IS_CAPSLOCK, 0xffe5, 0xffe5);

    // esc, tab, \,./;'[]-=`
    keys[ 1] = kk(AUTOREPEAT, 0xff1b, 0xff1b);
    keys[15] = kk(AUTOREPEAT, 0xff09, 0xff09);
    keys[43] = kk(AUTOREPEAT, 0x005c, 0x007c); // backslash
    keys[51] = kk(AUTOREPEAT, 0x002c, 0x003c); // ,
    keys[52] = kk(AUTOREPEAT, 0x002e, 0x003e); // .
    keys[53] = kk(AUTOREPEAT, 0x002f, 0x003f); // /
    keys[39] = kk(AUTOREPEAT, 0x003b, 0x003a); // ;
    keys[40] = kk(AUTOREPEAT, 0x0027, 0x0022); // '
    keys[26] = kk(AUTOREPEAT, 0x005b, 0x007b); // [
    keys[27] = kk(AUTOREPEAT, 0x005d, 0x007d); // [
    keys[12] = kk(AUTOREPEAT, 0x002d, 0x005f); // -
    keys[13] = kk(AUTOREPEAT, 0x003d, 0x002b); // =
    keys[41] = kk(AUTOREPEAT, 0x0060, 0x007); // `

    
    // del, back-space, home, end
    keys[83] = kk(AUTOREPEAT, 0xff9f, 0xff9f); // del
    keys[14] = kk(AUTOREPEAT, 0xff08, 0xff08); // backspace
    keys[71] = kk(AUTOREPEAT, 0xff95, 0xff95); // home
    keys[79] = kk(AUTOREPEAT, 0xff9c, 0xff9c); // end
    // pgup, pgdown
    keys[73] = kk(AUTOREPEAT, 0xff55, 0xff55);
    keys[81] = kk(AUTOREPEAT, 0xff56, 0xff56);
    // up,right,down,left
    keys[72] = kk(AUTOREPEAT, 0xff52, 0xff52);
    keys[77] = kk(AUTOREPEAT, 0xff53, 0xff53);
    keys[80] = kk(AUTOREPEAT, 0xff54, 0xff54);
    keys[75] = kk(AUTOREPEAT, 0xff51, 0xff51);
    // num-lock, sysrq, scroll lock, break
    keys[69] = kk(IS_NUMLOCK, 0xff7f, 0xff7f);
    keys[70] = kk(AUTOREPEAT, 0xff15, 0xff61);
    keys[71] = kk(AUTOREPEAT, 0xff14, 0xff14);
    keys[72] = kk(AUTOREPEAT, 0xff6b, 0xff13);
    
    return (tintptr) v;
}

/******************************************************************************/
int EXPORT_CC
mod_exit(tintptr handle)
{
    struct vnc *v = (struct vnc *) handle;
    LOG(LOG_LEVEL_TRACE, "VNC mod_exit");

    if (v == 0)
    {
        return 0;
    }
    trans_delete(v->trans);
    g_free(v->client_layout.s);
    g_free(v);
    return 0;
}
