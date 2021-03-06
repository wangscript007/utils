/**************************************************************************
 * Copyright (C) 2017-2017  Junlon2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **************************************************************************
 *
 * Description : uni_dns_parse.c
 * Author      : junlon2006@163.com
 * Date        : 2019.04.07
 *
 **************************************************************************/

#include "uni_dns_parse.h"

#include "uni_log.h"
#include "list_head.h"
#include "uni_interruptable.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DNS_PARSE_TAG         "dns_parse"
#define DNS_REFRESH_MSEC      (30 * 10 * 1000)

typedef struct {
  list_head link;
  char      *domain;
  uint64_t  ip;
} DnsParseItem;

typedef struct {
  list_head       list;
  int             running;
  pthread_t       pid;
  InterruptHandle interrupthandle;
  pthread_mutex_t mutex;
} DnsParse;

static DnsParse g_dns_parse;

static void _mutex_init() {
  pthread_mutex_init(&g_dns_parse.mutex, NULL);
}

static void _mutex_destroy() {
  pthread_mutex_destroy(&g_dns_parse.mutex);
}

static uint64_t _domain_2_ip(const char *domain) {
  struct hostent hostinfo, *host = NULL;
  char buf[1024];
  int err = 0;
  int ret = 0;
  ret = gethostbyname_r(domain, &hostinfo, buf, sizeof(buf), &host, &err);
  if (ret != 0 || err != 0 || NULL == host) {
    LOGW(DNS_PARSE_TAG, "dns parse failed[ret=%d, err=%d, host=%p]. domain=%s",
         ret, err, host, domain);
    return DNS_PARSE_INVALID_IP;
  }
  LOGT(DNS_PARSE_TAG, "get hostname=%s", host->h_name);
  LOGT(DNS_PARSE_TAG, "IP address=%s",
       inet_ntoa(*((struct in_addr *)host->h_addr)));
  return inet_addr(inet_ntoa(*((struct in_addr *)host->h_addr)));
}

static void *_refresh_dns_cache(void *args) {
  while (g_dns_parse.running) {
    DnsParseItem *p;
    uint64_t ip;
    InterruptableSleep(g_dns_parse.interrupthandle, DNS_REFRESH_MSEC);
    if (!g_dns_parse.running) {
      break;
    }
    pthread_mutex_lock(&g_dns_parse.mutex);
    list_for_each_entry(p, &g_dns_parse.list, DnsParseItem, link) {
      pthread_mutex_unlock(&g_dns_parse.mutex);
      ip = _domain_2_ip(p->domain);
      if (DNS_PARSE_INVALID_IP != ip) {
        pthread_mutex_lock(&g_dns_parse.mutex);
        p->ip = ip;
        pthread_mutex_unlock(&g_dns_parse.mutex);
      } else {
        usleep(1000 * 1000);
        InterruptableBreak(g_dns_parse.interrupthandle);
      }
      LOGT(DNS_PARSE_TAG, "refresh dns cache[%s]-->[%0x]", p->domain, p->ip);
      pthread_mutex_lock(&g_dns_parse.mutex);
    }
    pthread_mutex_unlock(&g_dns_parse.mutex);
  }
  return NULL;
}

static void _create_refresh_task() {
  g_dns_parse.running = 1;
  pthread_create(&g_dns_parse.pid, NULL, _refresh_dns_cache, NULL);
}

static void _destroy_list() {
  DnsParseItem *p, *n;
  list_for_each_entry_safe(p, n, &g_dns_parse.list, DnsParseItem, link) {
    list_del(&p->link);
    free(p->domain);
    free(p);
  }
}

static void _refresh_task_exit() {
  g_dns_parse.running = 0;
  InterruptableBreak(g_dns_parse.interrupthandle);
  pthread_join(g_dns_parse.pid, NULL);
}

int DnsParseInit(void) {
  memset(&g_dns_parse, 0, sizeof(DnsParse));
  list_init(&g_dns_parse.list);
  _mutex_init();
  g_dns_parse.interrupthandle = InterruptCreate();
  _create_refresh_task();
  return 0;
}

void DnsParseFinal(void) {
  _refresh_task_exit();
  _destroy_list();
  _mutex_destroy();
  InterruptDestroy(g_dns_parse.interrupthandle);
}

static int _find_exist_ip_by_domain(const char *domain, uint64_t *ip) {
  int ret = -1;
  DnsParseItem *p;
  pthread_mutex_lock(&g_dns_parse.mutex);
  list_for_each_entry(p, &g_dns_parse.list, DnsParseItem, link) {
    if (0 == strcmp(domain, p->domain)) {
      *ip = p->ip;
      ret = 0;
      LOGT(DNS_PARSE_TAG, "find domain[%s]--->[%0x]", domain, p->ip);
      break;
    }
  }
  pthread_mutex_unlock(&g_dns_parse.mutex);
  return ret;
}

static void _insert_new_dns_item_2_list(const char *domain, uint64_t *ip) {
  DnsParseItem *item = NULL;
  item = malloc(sizeof(DnsParseItem));
  item->domain = malloc(strlen(domain) + 1);
  strcpy(item->domain, domain);
  *ip = item->ip = _domain_2_ip(domain);
  pthread_mutex_lock(&g_dns_parse.mutex);
  list_add(&item->link, &g_dns_parse.list);
  pthread_mutex_unlock(&g_dns_parse.mutex);
}

uint64_t DnsParseByDomain(const char *domain) {
  uint64_t ip = DNS_PARSE_INVALID_IP;
  if (0 == _find_exist_ip_by_domain(domain, &ip)) {
    if (DNS_PARSE_INVALID_IP == ip) {
      InterruptableBreak(g_dns_parse.interrupthandle);
    }
    return ip;
  }
  _insert_new_dns_item_2_list(domain, &ip);
  return ip;
}
