/*
 *  tvheadend, channel functions
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <libhts/htscfg.h>

#include "tvhead.h"
#include "dvb.h"
#include "v4l.h"
#include "iptv_input.h"
#include "psi.h"
#include "channels.h"
#include "transports.h"

struct th_channel_list channels;
struct th_transport_list all_transports;
int nchannels;

struct th_channel_group_queue all_channel_groups;

th_channel_group_t *defgroup;

void scanner_init(void);

/**
 *
 */
th_channel_group_t *
channel_group_find(const char *name, int create)
{
  th_channel_group_t *tcg;

  TAILQ_FOREACH(tcg, &all_channel_groups, tcg_global_link) {
    if(!strcmp(name, tcg->tcg_name))
      return tcg;
  }
  if(!create)
    return NULL;

  tcg = calloc(1, sizeof(th_channel_group_t));
  tcg->tcg_name = strdup(name);
  tcg->tcg_tag = tag_get();
  
  TAILQ_INIT(&tcg->tcg_channels);

  TAILQ_INSERT_HEAD(&all_channel_groups, tcg, tcg_global_link);
  return tcg;
}




/**
 *
 */
void
channel_set_group(th_channel_t *ch, th_channel_group_t *tcg)
{
  if(ch->ch_group != NULL)
    TAILQ_REMOVE(&ch->ch_group->tcg_channels, ch, ch_group_link);

  ch->ch_group = tcg;
  TAILQ_INSERT_TAIL(&tcg->tcg_channels, ch, ch_group_link);
}

/**
 *
 */
void
channel_group_destroy(th_channel_group_t *tcg)
{
  th_channel_t *ch;

  if(defgroup == tcg)
    return;

  while((ch = TAILQ_FIRST(&tcg->tcg_channels)) != NULL) {
    channel_set_group(ch, defgroup);
  }

  TAILQ_REMOVE(&all_channel_groups, tcg, tcg_global_link);
  free((void *)tcg->tcg_name);
  free(tcg);
}

/**
 *
 */
th_channel_t *
channel_find(const char *name, int create)
{
  const char *n2;
  th_channel_t *ch;
  int l, i;
  char *cp, c;

  LIST_FOREACH(ch, &channels, ch_global_link)
    if(!strcasecmp(name, ch->ch_name))
      return ch;

  if(create == 0)
    return NULL;

  ch = calloc(1, sizeof(th_channel_t));
  ch->ch_name = strdup(name);

  l = strlen(name);
  ch->ch_sname = cp = malloc(l + 1);

  n2 = utf8toprintable(name);

  for(i = 0; i < strlen(n2); i++) {
    c = tolower(n2[i]);
    if(isalnum(c))
      *cp++ = c;
    else
      *cp++ = '-';
  }
  *cp = 0;

  free((void *)n2);

  ch->ch_index = nchannels;
  TAILQ_INIT(&ch->ch_epg_events);

  LIST_INSERT_HEAD(&channels, ch, ch_global_link);

  channel_set_group(ch, defgroup);

  ch->ch_tag = tag_get();
  nchannels++;
  return ch;
}


/**
 *
 */
static int
transportcmp(th_transport_t *a, th_transport_t *b)
{
  return a->tht_prio - b->tht_prio;
}


/**
 *
 */
int 
transport_set_channel(th_transport_t *t, th_channel_t *ch)
{
  th_stream_t *st;
  char *chname;
  const char *n;
  t->tht_channel = ch;
  LIST_INSERT_SORTED(&ch->ch_transports, t, tht_channel_link, transportcmp);

  chname = utf8toprintable(ch->ch_name);

  syslog(LOG_DEBUG, "Added service \"%s\" for channel \"%s\"",
	 t->tht_name, chname);
  free(chname);

  LIST_FOREACH(st, &t->tht_streams, st_link) {
    if(st->st_caid != 0) {
      n = psi_caid2name(st->st_caid);
    } else {
      n = htstvstreamtype2txt(st->st_type);
    }
    syslog(LOG_DEBUG, "   Stream [%s] - pid %d", n, st->st_pid);
  }

  return 0;
}




/**
 *
 */
static void
service_load(struct config_head *head)
{
  const char *name,  *v;
  th_transport_t *t;
  int r = 1;

  if((name = config_get_str_sub(head, "channel", NULL)) == NULL)
    return;

  t = calloc(1, sizeof(th_transport_t));

  t->tht_prio = atoi(config_get_str_sub(head, "prio", ""));

  if(0) {
#ifdef ENABLE_INPUT_DVB
  } else if((v = config_get_str_sub(head, "dvbmux", NULL)) != NULL) {
    r = dvb_configure_transport(t, v, name);
#endif
#ifdef ENABLE_INPUT_IPTV
  } else if((v = config_get_str_sub(head, "iptv", NULL)) != NULL) {
    r = iptv_configure_transport(t, v, head, name);
#endif
#ifdef ENABLE_INPUT_V4L
  } else if((v = config_get_str_sub(head, "v4lmux", NULL)) != NULL) {
    r = v4l_configure_transport(t, v, name);
#endif
  }
  if(r)
    free(t);
}

/**
 *
 */
void
transport_link(th_transport_t *t, th_channel_t *ch)
{
  transport_set_channel(t, ch);
  transport_monitor_init(t);
  LIST_INSERT_HEAD(&all_transports, t, tht_global_link);
}



/**
 *
 */
static void
channel_load(struct config_head *head)
{
  const char *name, *v;
  th_channel_t *ch;

  if((name = config_get_str_sub(head, "name", NULL)) == NULL)
    return;

  ch = channel_find(name, 1);

  syslog(LOG_DEBUG, "Added channel \"%s\"", name);

  if((v = config_get_str_sub(head, "teletext-rundown", NULL)) != NULL) {
    ch->ch_teletext_rundown = atoi(v);
  }
}


/**
 *
 */
void
channels_load(void)
{
  config_entry_t *ce;

  TAILQ_INIT(&all_channel_groups);

  defgroup = channel_group_find("Uncategorized", 1);
  defgroup->tcg_cant_delete_me = 1;

  TAILQ_FOREACH(ce, &config_list, ce_link) {
    if(ce->ce_type == CFG_SUB && !strcasecmp("channel", ce->ce_key)) {
      channel_load(&ce->ce_sub);
    }
  }

  TAILQ_FOREACH(ce, &config_list, ce_link) {
    if(ce->ce_type == CFG_SUB && !strcasecmp("service", ce->ce_key)) {
      service_load(&ce->ce_sub);
    }
  }
}


/**
 * The index stuff should go away
 */
th_channel_t *
channel_by_index(uint32_t index)
{
  th_channel_t *ch;

  LIST_FOREACH(ch, &channels, ch_global_link)
    if(ch->ch_index == index)
      return ch;

  return NULL;
}



/**
 *
 */
th_channel_t *
channel_by_tag(uint32_t tag)
{
  th_channel_t *ch;

  LIST_FOREACH(ch, &channels, ch_global_link)
    if(ch->ch_tag == tag)
      return ch;

  return NULL;
}



/**
 *
 */
th_channel_group_t *
channel_group_by_tag(uint32_t tag)
{
  th_channel_group_t *tcg;

  TAILQ_FOREACH(tcg, &all_channel_groups, tcg_global_link)
    if(tcg->tcg_tag == tag)
      return tcg;

  return NULL;
}
