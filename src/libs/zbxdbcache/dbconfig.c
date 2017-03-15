/*
** Zabbix
** Copyright (C) 2001-2017 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "log.h"
#include "threads.h"
#include "dbcache.h"
#include "ipc.h"
#include "mutexs.h"
#include "memalloc.h"
#include "strpool.h"
#include "zbxserver.h"
#include "zbxalgo.h"
#include "dbcache.h"
#include "zbxregexp.h"
#include "cfg.h"
#include "../zbxcrypto/tls_tcp_active.h"
#include "dbcache.h"

#define ZBX_DBCONFIG_IMPL
#include "dbconfig.h"
#include "dbsync.h"

static int	sync_in_progress = 0;

#define	LOCK_CACHE	if (0 == sync_in_progress) zbx_mutex_lock(&config_lock)
#define	UNLOCK_CACHE	if (0 == sync_in_progress) zbx_mutex_unlock(&config_lock)
#define START_SYNC	LOCK_CACHE; sync_in_progress = 1
#define FINISH_SYNC	sync_in_progress = 0; UNLOCK_CACHE

#define ZBX_LOC_NOWHERE	0
#define ZBX_LOC_QUEUE	1
#define ZBX_LOC_POLLER	2

#define ZBX_SNMP_OID_TYPE_NORMAL	0
#define ZBX_SNMP_OID_TYPE_DYNAMIC	1
#define ZBX_SNMP_OID_TYPE_MACRO		2

/* trigger is functional unless its expression contains disabled or not monitored items */
#define TRIGGER_FUNCTIONAL_TRUE		0
#define TRIGGER_FUNCTIONAL_FALSE	1

/* shorthand macro for calling in_maintenance_without_data_collection() */
#define DCin_maintenance_without_data_collection(dc_host, dc_item)			\
		in_maintenance_without_data_collection(dc_host->maintenance_status,	\
				dc_host->maintenance_type, dc_item->type)

/* validator function optionally used to validate macro values when expanding user macros */

/******************************************************************************
 *                                                                            *
 * Function: zbx_value_validator_func_t                                       *
 *                                                                            *
 * Purpose: validate macro value when expanding user macros                   *
 *                                                                            *
 * Parameters: macro   - [IN] the user macro                                  *
 *             value   - [IN] the macro value                                 *
 *             error   - [OUT] the error message                              *
 *                                                                            *
 * Return value: SUCCEED - the value is valid                                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
typedef int (*zbx_value_validator_func_t)(const char *macro, const char *value, char **error);

static ZBX_DC_CONFIG	*config = NULL;
static ZBX_MUTEX	config_lock = ZBX_MUTEX_NULL;
static zbx_mem_info_t	*config_mem;

extern unsigned char	program_type;
extern int		CONFIG_TIMER_FORKS;

ZBX_MEM_FUNC_IMPL(__config, config_mem)

static void	dc_get_hostids_by_functionids(zbx_vector_uint64_t *functionids, zbx_vector_uint64_t *hostids);
static char	*dc_cache_expanded_expression(const char *expression, const char **expression_ex, char **error);

/******************************************************************************
 *                                                                            *
 * Function: is_item_processed_by_server                                      *
 *                                                                            *
 * Parameters: type - [IN] item type [ITEM_TYPE_* flag]                       *
 *             key  - [IN] item key                                           *
 *                                                                            *
 * Return value: SUCCEED when an item should be processed by server           *
 *               FAIL otherwise                                               *
 *                                                                            *
 * Comments: list of the items, always processed by server                    *
 *           ,------------------+--------------------------------------,      *
 *           | type             | key                                  |      *
 *           +------------------+--------------------------------------+      *
 *           | Zabbix internal  | zabbix[host,,items]                  |      *
 *           | Zabbix internal  | zabbix[host,,items_unsupported]      |      *
 *           | Zabbix internal  | zabbix[host,,maintenance]            |      *
 *           | Zabbix internal  | zabbix[proxy,<proxyname>,lastaccess] |      *
 *           | Zabbix aggregate | *                                    |      *
 *           | Calculated       | *                                    |      *
 *           '------------------+--------------------------------------'      *
 *                                                                            *
 ******************************************************************************/
int	is_item_processed_by_server(unsigned char type, const char *key)
{
	int	ret = FAIL;

	switch (type)
	{
		case ITEM_TYPE_AGGREGATE:
		case ITEM_TYPE_CALCULATED:
			ret = SUCCEED;
			break;

		case ITEM_TYPE_INTERNAL:
			if (0 == strncmp(key, "zabbix[", 7))
			{
				AGENT_REQUEST	request;
				char		*arg1, *arg2, *arg3;

				init_request(&request);

				if (SUCCEED != parse_item_key(key, &request) || 3 != request.nparam)
					goto clean;

				arg1 = get_rparam(&request, 0);

				if (0 == strcmp(arg1, "host"))
				{
					arg2 = get_rparam(&request, 1);
					arg3 = get_rparam(&request, 2);

					if ((0 != strcmp(arg3, "maintenance") && 0 != strcmp(arg3, "items") &&
							0 != strcmp(arg3, "items_unsupported")) || '\0' != *arg2)
					{
						goto clean;
					}
				}
				else if (0 == strcmp(arg1, "proxy"))
				{
					arg3 = get_rparam(&request, 2);

					if (0 != strcmp(arg3, "lastaccess"))
						goto clean;
				}
				else
					goto clean;

				ret = SUCCEED;
clean:
				free_request(&request);
			}
			break;
	}

	return ret;
}

static unsigned char	poller_by_item(zbx_uint64_t proxy_hostid, unsigned char type, const char *key)
{
	if (0 != proxy_hostid && SUCCEED != is_item_processed_by_server(type, key))
		return ZBX_NO_POLLER;

	switch (type)
	{
		case ITEM_TYPE_SIMPLE:
			if (SUCCEED == cmp_key_id(key, SERVER_ICMPPING_KEY) ||
					SUCCEED == cmp_key_id(key, SERVER_ICMPPINGSEC_KEY) ||
					SUCCEED == cmp_key_id(key, SERVER_ICMPPINGLOSS_KEY))
			{
				if (0 == CONFIG_PINGER_FORKS)
					break;

				return ZBX_POLLER_TYPE_PINGER;
			}
			/* break; is not missing here */
		case ITEM_TYPE_ZABBIX:
		case ITEM_TYPE_SNMPv1:
		case ITEM_TYPE_SNMPv2c:
		case ITEM_TYPE_SNMPv3:
		case ITEM_TYPE_INTERNAL:
		case ITEM_TYPE_AGGREGATE:
		case ITEM_TYPE_EXTERNAL:
		case ITEM_TYPE_DB_MONITOR:
		case ITEM_TYPE_SSH:
		case ITEM_TYPE_TELNET:
		case ITEM_TYPE_CALCULATED:
			if (0 == CONFIG_POLLER_FORKS)
				break;

			return ZBX_POLLER_TYPE_NORMAL;
		case ITEM_TYPE_IPMI:
			if (0 == CONFIG_IPMIPOLLER_FORKS)
				break;

			return ZBX_POLLER_TYPE_IPMI;
		case ITEM_TYPE_JMX:
			if (0 == CONFIG_JAVAPOLLER_FORKS)
				break;

			return ZBX_POLLER_TYPE_JAVA;
	}

	return ZBX_NO_POLLER;
}

/******************************************************************************
 *                                                                            *
 * Function: is_counted_in_item_queue                                         *
 *                                                                            *
 * Purpose: determine whether the given item type is counted in item queue    *
 *                                                                            *
 * Return value: SUCCEED if item is counted in the queue, FAIL otherwise      *
 *                                                                            *
 ******************************************************************************/
static int	is_counted_in_item_queue(unsigned char type, const char *key)
{
	switch (type)
	{
		case ITEM_TYPE_ZABBIX_ACTIVE:
			if (0 == strncmp(key, "log[", 4) ||
					0 == strncmp(key, "logrt[", 6) ||
					0 == strncmp(key, "eventlog[", 9))
			{
				return FAIL;
			}
			break;
		case ITEM_TYPE_TRAPPER:
		case ITEM_TYPE_HTTPTEST:
		case ITEM_TYPE_SNMPTRAP:
			return FAIL;
	}

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: get_item_nextcheck_seed                                          *
 *                                                                            *
 * Purpose: get the seed value to be used for item nextcheck calculations     *
 *                                                                            *
 * Return value: the seed for nextcheck calculations                          *
 *                                                                            *
 * Comments: The seed value is used to spread multiple item nextchecks over   *
 *           the item delay period to even the system load.                   *
 *           Items with the same delay period and seed value will have the    *
 *           same nextcheck values.                                           *
 *                                                                            *
 ******************************************************************************/
static zbx_uint64_t	get_item_nextcheck_seed(zbx_uint64_t itemid, zbx_uint64_t interfaceid, unsigned char type,
		const char *key)
{
	if (ITEM_TYPE_JMX == type)
		return interfaceid;

	if (SUCCEED == is_snmp_type(type))
	{
		ZBX_DC_INTERFACE	*interface;

		if (NULL == (interface = zbx_hashset_search(&config->interfaces, &interfaceid)) ||
				SNMP_BULK_ENABLED != interface->bulk)
		{
			return itemid;
		}

		return interfaceid;
	}

	if (ITEM_TYPE_SIMPLE == type)
	{
		if (SUCCEED == cmp_key_id(key, SERVER_ICMPPING_KEY) ||
				SUCCEED == cmp_key_id(key, SERVER_ICMPPINGSEC_KEY) ||
				SUCCEED == cmp_key_id(key, SERVER_ICMPPINGLOSS_KEY))
		{
			return interfaceid;
		}
	}

	return itemid;
}

static int	DCget_reachable_nextcheck(const ZBX_DC_ITEM *item, const ZBX_DC_HOST *host, int now)
{
	ZBX_DC_PROXY	*proxy = NULL;
	zbx_uint64_t	seed;
	int		nextcheck;

	if (0 != host->proxy_hostid && NULL != (proxy = zbx_hashset_search(&config->proxies, &host->proxy_hostid)))
		now -= proxy->timediff;

	seed = get_item_nextcheck_seed(item->itemid, item->interfaceid, item->type, item->key);

	if (ITEM_STATE_NOTSUPPORTED == item->state)
	{
		nextcheck = calculate_item_nextcheck(seed, item->type, config->config->refresh_unsupported, NULL, now);
	}
	else
	{
		const ZBX_DC_FLEXITEM	*flexitem;
		const char		*delay_flex;

		flexitem = zbx_hashset_search(&config->flexitems, &item->itemid);
		delay_flex = (NULL != flexitem ? flexitem->delay_flex : NULL);

		nextcheck = calculate_item_nextcheck(seed, item->type, item->delay, delay_flex, now);
	}

	if (NULL != proxy)
		nextcheck += proxy->timediff + 1;

	return nextcheck;
}

static int	DCget_unreachable_nextcheck(const ZBX_DC_ITEM *item, const ZBX_DC_HOST *host)
{
	switch (item->type)
	{
		case ITEM_TYPE_ZABBIX:
			if (0 != host->errors_from)
				return host->disable_until;
			break;
		case ITEM_TYPE_SNMPv1:
		case ITEM_TYPE_SNMPv2c:
		case ITEM_TYPE_SNMPv3:
			if (0 != host->snmp_errors_from)
				return host->snmp_disable_until;
			break;
		case ITEM_TYPE_IPMI:
			if (0 != host->ipmi_errors_from)
				return host->ipmi_disable_until;
			break;
		case ITEM_TYPE_JMX:
			if (0 != host->jmx_errors_from)
				return host->jmx_disable_until;
			break;
		default:
			/* nothing to do */;
	}

	return DCget_reachable_nextcheck(item, host, time(NULL));
}

static int	DCget_disable_until(const ZBX_DC_ITEM *item, const ZBX_DC_HOST *host)
{
	switch (item->type)
	{
		case ITEM_TYPE_ZABBIX:
			if (0 != host->errors_from)
				return host->disable_until;
			break;
		case ITEM_TYPE_SNMPv1:
		case ITEM_TYPE_SNMPv2c:
		case ITEM_TYPE_SNMPv3:
			if (0 != host->snmp_errors_from)
				return host->snmp_disable_until;
			break;
		case ITEM_TYPE_IPMI:
			if (0 != host->ipmi_errors_from)
				return host->ipmi_disable_until;
			break;
		case ITEM_TYPE_JMX:
			if (0 != host->jmx_errors_from)
				return host->jmx_disable_until;
			break;
		default:
			/* nothing to do */;
	}

	return 0;
}

static void	DCincrease_disable_until(const ZBX_DC_ITEM *item, ZBX_DC_HOST *host, int now)
{
	switch (item->type)
	{
		case ITEM_TYPE_ZABBIX:
			if (0 != host->errors_from)
				host->disable_until = now + CONFIG_TIMEOUT;
			break;
		case ITEM_TYPE_SNMPv1:
		case ITEM_TYPE_SNMPv2c:
		case ITEM_TYPE_SNMPv3:
			if (0 != host->snmp_errors_from)
				host->snmp_disable_until = now + CONFIG_TIMEOUT;
			break;
		case ITEM_TYPE_IPMI:
			if (0 != host->ipmi_errors_from)
				host->ipmi_disable_until = now + CONFIG_TIMEOUT;
			break;
		case ITEM_TYPE_JMX:
			if (0 != host->jmx_errors_from)
				host->jmx_disable_until = now + CONFIG_TIMEOUT;
			break;
		default:
			/* nothing to do */;
	}
}

static void	*DCfind_id(zbx_hashset_t *hashset, zbx_uint64_t id, size_t size, int *found)
{
	void		*ptr;
	zbx_uint64_t	buffer[1024];	/* adjust buffer size to accommodate any type DCfind_id() can be called for */

	if (NULL == (ptr = zbx_hashset_search(hashset, &id)))
	{
		*found = 0;

		buffer[0] = id;
		ptr = zbx_hashset_insert(hashset, &buffer[0], size);
	}
	else
		*found = 1;

	return ptr;
}

static ZBX_DC_ITEM	*DCfind_item(zbx_uint64_t hostid, const char *key)
{
	ZBX_DC_ITEM_HK	*item_hk, item_hk_local;

	item_hk_local.hostid = hostid;
	item_hk_local.key = key;

	if (NULL == (item_hk = zbx_hashset_search(&config->items_hk, &item_hk_local)))
		return NULL;
	else
		return item_hk->item_ptr;
}

static ZBX_DC_HOST	*DCfind_host(const char *host)
{
	ZBX_DC_HOST_H	*host_h, host_h_local;

	host_h_local.host = host;

	if (NULL == (host_h = zbx_hashset_search(&config->hosts_h, &host_h_local)))
		return NULL;
	else
		return host_h->host_ptr;
}

/******************************************************************************
 *                                                                            *
 * Function: DCfind_proxy                                                     *
 *                                                                            *
 * Purpose: Find a record with proxy details in configuration cache using the *
 *          proxy name                                                        *
 *                                                                            *
 * Parameters: host - [IN] proxy name                                         *
 *                                                                            *
 * Return value: pointer to record if found or NULL otherwise                 *
 *                                                                            *
 ******************************************************************************/
static ZBX_DC_HOST	*DCfind_proxy(const char *host)
{
	ZBX_DC_HOST_H	*host_p, host_p_local;

	host_p_local.host = host;

	if (NULL == (host_p = zbx_hashset_search(&config->hosts_p, &host_p_local)))
		return NULL;
	else
		return host_p->host_ptr;
}

static int	DCstrpool_replace(int found, const char **curr, const char *new)
{
	if (1 == found)
	{
		if (0 == strcmp(*curr, new))
			return FAIL;

		zbx_strpool_release(*curr);
	}

	*curr = zbx_strpool_intern(new);

	return SUCCEED;	/* indicate that the string has been replaced */
}

static void	DCupdate_item_queue(ZBX_DC_ITEM *item, unsigned char old_poller_type, int old_nextcheck)
{
	zbx_binary_heap_elem_t	elem;

	if (ZBX_LOC_POLLER == item->location)
		return;

	if (ZBX_LOC_QUEUE == item->location && old_poller_type != item->poller_type)
	{
		item->location = ZBX_LOC_NOWHERE;
		zbx_binary_heap_remove_direct(&config->queues[old_poller_type], item->itemid);
	}

	if (item->poller_type >= ZBX_POLLER_TYPE_COUNT)
		return;

	if (ZBX_LOC_QUEUE == item->location && old_nextcheck == item->nextcheck)
		return;

	elem.key = item->itemid;
	elem.data = (const void *)item;

	if (ZBX_LOC_QUEUE != item->location)
	{
		item->location = ZBX_LOC_QUEUE;
		zbx_binary_heap_insert(&config->queues[item->poller_type], &elem);
	}
	else
		zbx_binary_heap_update_direct(&config->queues[item->poller_type], &elem);
}

static void	DCupdate_proxy_queue(ZBX_DC_PROXY *proxy)
{
	zbx_binary_heap_elem_t	elem;

	if (ZBX_LOC_POLLER == proxy->location)
		return;

	elem.key = proxy->hostid;
	elem.data = (const void *)proxy;

	if (ZBX_LOC_QUEUE != proxy->location)
	{
		proxy->location = ZBX_LOC_QUEUE;
		zbx_binary_heap_insert(&config->pqueue, &elem);
	}
	else
		zbx_binary_heap_update_direct(&config->pqueue, &elem);
}

/******************************************************************************
 *                                                                            *
 * Function: config_gmacro_add_index                                          *
 *                                                                            *
 * Purpose: adds global macro index                                           *
 *                                                                            *
 * Parameters: gmacro_index - [IN/OUT] a global macro index hashset           *
 *             gmacro       - [IN] the macro to index                         *
 *                                                                            *
 ******************************************************************************/
static void	config_gmacro_add_index(zbx_hashset_t *gmacro_index, ZBX_DC_GMACRO *gmacro)
{
	ZBX_DC_GMACRO_M	*gmacro_m, gmacro_m_local;

	gmacro_m_local.macro = gmacro->macro;

	if (NULL == (gmacro_m = zbx_hashset_search(gmacro_index, &gmacro_m_local)))
	{
		gmacro_m_local.macro = zbx_strpool_acquire(gmacro->macro);
		zbx_vector_ptr_create_ext(&gmacro_m_local.gmacros, __config_mem_malloc_func, __config_mem_realloc_func,
				__config_mem_free_func);

		gmacro_m = zbx_hashset_insert(gmacro_index, &gmacro_m_local, sizeof(ZBX_DC_GMACRO_M));
	}

	zbx_vector_ptr_append(&gmacro_m->gmacros, gmacro);
}

/******************************************************************************
 *                                                                            *
 * Function: config_gmacro_remove_index                                       *
 *                                                                            *
 * Purpose: removes global macro index                                        *
 *                                                                            *
 * Parameters: gmacro_index - [IN/OUT] a global macro index hashset           *
 *             gmacro       - [IN] the macro to remove                        *
 *                                                                            *
 ******************************************************************************/
static void	config_gmacro_remove_index(zbx_hashset_t *gmacro_index, ZBX_DC_GMACRO *gmacro)
{
	ZBX_DC_GMACRO_M	*gmacro_m, gmacro_m_local;
	int		index;

	gmacro_m_local.macro = gmacro->macro;

	if (NULL != (gmacro_m = zbx_hashset_search(gmacro_index, &gmacro_m_local)))
	{
		if (FAIL != (index = zbx_vector_ptr_search(&gmacro_m->gmacros, gmacro, ZBX_DEFAULT_PTR_COMPARE_FUNC)))
			zbx_vector_ptr_remove(&gmacro_m->gmacros, index);

		if (0 == gmacro_m->gmacros.values_num)
		{
			zbx_strpool_release(gmacro_m->macro);
			zbx_vector_ptr_destroy(&gmacro_m->gmacros);
			zbx_hashset_remove(gmacro_index, &gmacro_m_local);
		}
	}
}

/******************************************************************************
 *                                                                            *
 * Function: config_hmacro_add_index                                          *
 *                                                                            *
 * Purpose: adds host macro index                                             *
 *                                                                            *
 * Parameters: hmacro_index - [IN/OUT] a host macro index hashset             *
 *             hmacro       - [IN] the macro to index                         *
 *                                                                            *
 ******************************************************************************/
static void	config_hmacro_add_index(zbx_hashset_t *hmacro_index, ZBX_DC_HMACRO *hmacro)
{
	ZBX_DC_HMACRO_HM	*hmacro_hm, hmacro_hm_local;

	hmacro_hm_local.hostid = hmacro->hostid;
	hmacro_hm_local.macro = hmacro->macro;

	if (NULL == (hmacro_hm = zbx_hashset_search(hmacro_index, &hmacro_hm_local)))
	{
		hmacro_hm_local.macro = zbx_strpool_acquire(hmacro->macro);
		zbx_vector_ptr_create_ext(&hmacro_hm_local.hmacros, __config_mem_malloc_func, __config_mem_realloc_func,
				__config_mem_free_func);

		hmacro_hm = zbx_hashset_insert(hmacro_index, &hmacro_hm_local, sizeof(ZBX_DC_HMACRO_HM));
	}

	zbx_vector_ptr_append(&hmacro_hm->hmacros, hmacro);
}

/******************************************************************************
 *                                                                            *
 * Function: config_hmacro_remove_index                                       *
 *                                                                            *
 * Purpose: removes host macro index                                          *
 *                                                                            *
 * Parameters: hmacro_index - [IN/OUT] a host macro index hashset             *
 *             hmacro       - [IN] the macro name to remove                   *
 *                                                                            *
 ******************************************************************************/
static void	config_hmacro_remove_index(zbx_hashset_t *hmacro_index, ZBX_DC_HMACRO *hmacro)
{
	ZBX_DC_HMACRO_HM	*hmacro_hm, hmacro_hm_local;
	int			index;

	hmacro_hm_local.hostid = hmacro->hostid;
	hmacro_hm_local.macro = hmacro->macro;

	if (NULL != (hmacro_hm = zbx_hashset_search(hmacro_index, &hmacro_hm_local)))
	{
		if (FAIL != (index = zbx_vector_ptr_search(&hmacro_hm->hmacros, hmacro, ZBX_DEFAULT_PTR_COMPARE_FUNC)))
			zbx_vector_ptr_remove(&hmacro_hm->hmacros, index);

		if (0 == hmacro_hm->hmacros.values_num)
		{
			zbx_strpool_release(hmacro_hm->macro);
			zbx_vector_ptr_destroy(&hmacro_hm->hmacros);
			zbx_hashset_remove(hmacro_index, &hmacro_hm_local);
		}
	}
}

static int	DCsync_config(zbx_dbsync_t *sync, int *refresh_unsupported_changed)
{
	static char	*default_severity_names[] = {"Not classified", "Information", "Warning", "Average", "High", "Disaster"};
	const char	*__function_name = "DCsync_config";
	int		i, found = 1;
	char		**row;
	zbx_uint64_t	rowid;
	unsigned char	tag;

#define DEFAULT_REFRESH_UNSUPPORTED	600

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	*refresh_unsupported_changed = 0;

	if (NULL == config->config)
	{
		found = 0;
		config->config = __config_mem_malloc_func(NULL, sizeof(ZBX_DC_CONFIG_TABLE));
	}

	if (FAIL == zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (0 != (program_type & ZBX_PROGRAM_TYPE_SERVER))
			zabbix_log(LOG_LEVEL_ERR, "no records in table 'config'");

		if (0 == found)
		{
			/* load default config data */

			*refresh_unsupported_changed = 1;

			config->config->refresh_unsupported = DEFAULT_REFRESH_UNSUPPORTED;
			config->config->discovery_groupid = 0;
			config->config->snmptrap_logging = ZBX_SNMPTRAP_LOGGING_ENABLED;
			config->config->default_inventory_mode = HOST_INVENTORY_DISABLED;

			for (i = 0; TRIGGER_SEVERITY_COUNT > i; i++)
				DCstrpool_replace(found, &config->config->severity_name[i], default_severity_names[i]);

			/* set default housekeeper configuration */
			config->config->hk.events_mode = ZBX_HK_OPTION_ENABLED;
			config->config->hk.events_trigger = 365;
			config->config->hk.events_internal = 365;
			config->config->hk.events_autoreg = 365;
			config->config->hk.events_discovery = 365;

			config->config->hk.audit_mode = ZBX_HK_OPTION_ENABLED;
			config->config->hk.audit = 365;

			config->config->hk.services_mode = ZBX_HK_OPTION_ENABLED;
			config->config->hk.services = 365;

			config->config->hk.sessions_mode = ZBX_HK_OPTION_ENABLED;
			config->config->hk.sessions = 365;

			config->config->hk.history_mode = ZBX_HK_OPTION_ENABLED;
			config->config->hk.history_global = ZBX_HK_OPTION_DISABLED;
			config->config->hk.history = 90;

			config->config->hk.trends_mode = ZBX_HK_OPTION_ENABLED;
			config->config->hk.trends_global = ZBX_HK_OPTION_DISABLED;
			config->config->hk.trends = 365;
		}
	}
	else
	{
		int	refresh_unsupported;

		/* store the config data */

		refresh_unsupported = atoi(row[0]);

		if (0 == found || config->config->refresh_unsupported != refresh_unsupported)
			*refresh_unsupported_changed = 1;

		config->config->refresh_unsupported = refresh_unsupported;
		ZBX_STR2UINT64(config->config->discovery_groupid, row[1]);
		config->config->snmptrap_logging = (unsigned char)atoi(row[2]);
		config->config->default_inventory_mode = atoi(row[26]);

		for (i = 0; TRIGGER_SEVERITY_COUNT > i; i++)
			DCstrpool_replace(found, &config->config->severity_name[i], row[3 + i]);

		/* read housekeeper configuration */
		config->config->hk.events_mode = atoi(row[9]);
		config->config->hk.events_trigger = atoi(row[10]);
		config->config->hk.events_internal = atoi(row[11]);
		config->config->hk.events_discovery = atoi(row[12]);
		config->config->hk.events_autoreg = atoi(row[13]);

		config->config->hk.services_mode = atoi(row[14]);
		config->config->hk.services = atoi(row[15]);

		config->config->hk.audit_mode = atoi(row[16]);
		config->config->hk.audit = atoi(row[17]);

		config->config->hk.sessions_mode = atoi(row[18]);
		config->config->hk.sessions = atoi(row[19]);

		config->config->hk.history_mode = atoi(row[20]);
		config->config->hk.history = atoi(row[22]);

		if (ZBX_HK_OPTION_ENABLED == config->config->hk.history_mode)
			config->config->hk.history_global = atoi(row[21]);
		else
			config->config->hk.history_global = ZBX_HK_OPTION_DISABLED;

		config->config->hk.trends_mode = atoi(row[23]);
		config->config->hk.trends = atoi(row[25]);

		if (ZBX_HK_OPTION_ENABLED == config->config->hk.trends_mode)
			config->config->hk.trends_global = atoi(row[24]);
		else
			config->config->hk.trends_global = ZBX_HK_OPTION_DISABLED;

		if (FAIL != zbx_dbsync_next(sync, &rowid, &row, &tag))
			zabbix_log(LOG_LEVEL_ERR, "table 'config' has multiple records");
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

#undef DEFAULT_REFRESH_UNSUPPORTED

	return SUCCEED;
}

static void	DCsync_hosts(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_hosts";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_HOST		*host;
	ZBX_DC_IPMIHOST		*ipmihost;
	ZBX_DC_PROXY		*proxy;
	ZBX_DC_HOST_H		*host_h, host_h_local, *host_p, host_p_local;

	int			found;
	int			update_index_h, update_index_p, ret;
	zbx_uint64_t		hostid, proxy_hostid;
	unsigned char		status;
	time_t			now;
	signed char		ipmi_authtype;
	unsigned char		ipmi_privilege;
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	ZBX_DC_PSK		*psk_i, psk_i_local;
	zbx_ptr_pair_t		*psk_owner, psk_owner_local;
	zbx_hashset_t		psk_owners;
#endif
	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	zbx_hashset_create(&psk_owners, 0, ZBX_DEFAULT_PTR_HASH_FUNC, ZBX_DEFAULT_PTR_COMPARE_FUNC);
#endif
	now = time(NULL);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(hostid, row[0]);
		ZBX_DBROW2UINT64(proxy_hostid, row[1]);
		ZBX_STR2UCHAR(status, row[22]);

		host = DCfind_id(&config->hosts, hostid, sizeof(ZBX_DC_HOST), &found);

		/* see whether we should and can update 'hosts_h' and 'hosts_p' indexes at this point */

		update_index_h = 0;
		update_index_p = 0;

		if ((HOST_STATUS_MONITORED == status || HOST_STATUS_NOT_MONITORED == status) &&
				(0 == found || 0 != strcmp(host->host, row[2])))
		{
			if (1 == found)
			{
				host_h_local.host = host->host;
				host_h = zbx_hashset_search(&config->hosts_h, &host_h_local);

				if (NULL != host_h && host == host_h->host_ptr)	/* see ZBX-4045 for NULL check */
				{
					zbx_strpool_release(host_h->host);
					zbx_hashset_remove_direct(&config->hosts_h, host_h);
				}
			}

			host_h_local.host = row[2];
			host_h = zbx_hashset_search(&config->hosts_h, &host_h_local);

			if (NULL != host_h)
				host_h->host_ptr = host;
			else
				update_index_h = 1;
		}
		else if ((HOST_STATUS_PROXY_ACTIVE == status || HOST_STATUS_PROXY_PASSIVE == status) &&
				(0 == found || 0 != strcmp(host->host, row[2])))
		{
			if (1 == found)
			{
				host_p_local.host = host->host;
				host_p = zbx_hashset_search(&config->hosts_p, &host_p_local);

				if (NULL != host_p && host == host_p->host_ptr)
				{
					zbx_strpool_release(host_p->host);
					zbx_hashset_remove_direct(&config->hosts_p, host_p);
				}
			}

			host_p_local.host = row[2];
			host_p = zbx_hashset_search(&config->hosts_p, &host_p_local);

			if (NULL != host_p)
				host_p->host_ptr = host;
			else
				update_index_p = 1;
		}

		/* store new information in host structure */

		/* reset the used interfaces flag */
		host->used_interfaces = ZBX_FLAG_INTERFACE_NONE;

		DCstrpool_replace(found, &host->host, row[2]);
		DCstrpool_replace(found, &host->name, row[23]);
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
		DCstrpool_replace(found, &host->tls_issuer, row[31]);
		DCstrpool_replace(found, &host->tls_subject, row[32]);

		/* maintain 'config->psks' in configuration cache */

		/*****************************************************************************/
		/*                                                                           */
		/* cases to cover (PSKid means PSK identity):                                */
		/*                                                                           */
		/*                                  Incoming data record                     */
		/*                                  /                   \                    */
		/*                                new                   new                  */
		/*                               PSKid                 PSKid                 */
		/*                             non-empty               empty                 */
		/*                             /      \                /    \                */
		/*                            /        \              /      \               */
		/*                       'host'        'host'      'host'    'host'          */
		/*                       record        record      record    record          */
		/*                        has           has         has       has            */
		/*                     non-empty       empty     non-empty  empty PSK        */
		/*                        PSK           PSK         PSK      |     \         */
		/*                       /   \           |           |       |      \        */
		/*                      /     \          |           |       |       \       */
		/*                     /       \         |           |       |        \      */
		/*            new PSKid       new PSKid  |           |   existing     new    */
		/*             same as         differs   |           |    record     record  */
		/*            old PSKid         from     |           |      |          |     */
		/*           /    |           old PSKid  |           |     done        |     */
		/*          /     |              |       |           |                 |     */
		/*   new PSK    new PSK        delete    |        delete               |     */
		/*    value      value        old PSKid  |       old PSKid             |     */
		/*   same as    differs       and value  |       and value             |     */
		/*     old       from         from psks  |       from psks             |     */
		/*      |        old          hashset    |        hashset              |     */
		/*     done       /           (if ref    |        (if ref              |     */
		/*               /            count=0)   |        count=0)             |     */
		/*              /              /     \  /|           \                /      */
		/*             /              /--------- |            \              /       */
		/*            /              /         \ |             \            /        */
		/*       delete          new PSKid   new PSKid         set pointer in        */
		/*       old PSK          already     not in           'hosts' record        */
		/*        value           in psks      psks             to NULL PSK          */
		/*        from            hashset     hashset                |               */
		/*       string            /   \          \                 done             */
		/*        pool            /     \          \                                 */
		/*         |             /       \          \                                */
		/*       change    PSK value   PSK value    insert                           */
		/*      PSK value  in hashset  in hashset  new PSKid                         */
		/*      for this    same as     differs    and value                         */
		/*       PSKid      new PSK     from new   into psks                         */
		/*         |        value      PSK value    hashset                          */
		/*        done        \           |            /                             */
		/*                     \       replace        /                              */
		/*                      \      PSK value     /                               */
		/*                       \     in hashset   /                                */
		/*                        \    with new    /                                 */
		/*                         \   PSK value  /                                  */
		/*                          \     |      /                                   */
		/*                           \    |     /                                    */
		/*                            set pointer                                    */
		/*                            in 'host'                                      */
		/*                            record to                                      */
		/*                            new PSKid                                      */
		/*                                |                                          */
		/*                               done                                        */
		/*                                                                           */
		/*****************************************************************************/

		psk_owner = NULL;

		if ('\0' == *row[33] || '\0' == *row[34])	/* new PSKid or value empty */
		{
			/* In case of "impossible" errors ("PSK value without identity" or "PSK identity without */
			/* value") assume empty PSK identity and value. These errors should have been prevented */
			/* by validation in frontend/API. Be prepared when making a connection requiring PSK - */
			/* the PSK might not be available. */

			if (1 == found)
			{
				if (NULL == host->tls_dc_psk)	/* 'host' record has empty PSK */
					goto done;

				/* 'host' record has non-empty PSK. Unlink and delete PSK. */

				psk_i_local.tls_psk_identity = host->tls_dc_psk->tls_psk_identity;

				if (NULL != (psk_i = zbx_hashset_search(&config->psks, &psk_i_local)) &&
						0 == --(psk_i->refcount))
				{
					zbx_strpool_release(psk_i->tls_psk_identity);
					zbx_strpool_release(psk_i->tls_psk);
					zbx_hashset_remove_direct(&config->psks, psk_i);
				}
			}

			host->tls_dc_psk = NULL;
			goto done;
		}

		/* new PSKid and value non-empty */

		zbx_strlower(row[34]);

		if (1 == found && NULL != host->tls_dc_psk)	/* 'host' record has non-empty PSK */
		{
			if (0 == strcmp(host->tls_dc_psk->tls_psk_identity, row[33]))	/* new PSKid same as */
											/* old PSKid */
			{
				if (0 != strcmp(host->tls_dc_psk->tls_psk, row[34]))	/* new PSK value */
											/* differs from old */
				{
					if (NULL == (psk_owner = zbx_hashset_search(&psk_owners,
							&host->tls_dc_psk->tls_psk_identity)))
					{
						/* change underlying PSK value and 'config->psks' is updated, too */
						DCstrpool_replace(1, &host->tls_dc_psk->tls_psk, row[34]);
					}
					else
					{
						zabbix_log(LOG_LEVEL_WARNING, "conflicting PSK values for PSK identity"
								" \"%s\" on hosts \"%s\" and \"%s\" (and maybe others)",
								psk_owner->first, psk_owner->second, host->host);
					}
				}

				goto done;
			}

			/* New PSKid differs from old PSKid. Unlink and delete old PSK. */

			psk_i_local.tls_psk_identity = host->tls_dc_psk->tls_psk_identity;

			if (NULL != (psk_i = zbx_hashset_search(&config->psks, &psk_i_local)) &&
					0 == --(psk_i->refcount))
			{
				zbx_strpool_release(psk_i->tls_psk_identity);
				zbx_strpool_release(psk_i->tls_psk);
				zbx_hashset_remove_direct(&config->psks, psk_i);
			}

			host->tls_dc_psk = NULL;
		}

		/* new PSK identity already stored? */

		psk_i_local.tls_psk_identity = row[33];

		if (NULL != (psk_i = zbx_hashset_search(&config->psks, &psk_i_local)))
		{
			/* new PSKid already in psks hashset */

			if (0 != strcmp(psk_i->tls_psk, row[34]))	/* PSKid stored but PSK value is different */
			{
				if (NULL == (psk_owner = zbx_hashset_search(&psk_owners, &psk_i->tls_psk_identity)))
				{
					DCstrpool_replace(1, &psk_i->tls_psk, row[34]);
				}
				else
				{
					zabbix_log(LOG_LEVEL_WARNING, "conflicting PSK values for PSK identity"
							" \"%s\" on hosts \"%s\" and \"%s\" (and maybe others)",
							psk_owner->first, psk_owner->second, host->host);
				}
			}

			host->tls_dc_psk = psk_i;
			psk_i->refcount++;
			goto done;
		}

		/* insert new PSKid and value into psks hashset */

		DCstrpool_replace(0, &psk_i_local.tls_psk_identity, row[33]);
		DCstrpool_replace(0, &psk_i_local.tls_psk, row[34]);
		psk_i_local.refcount = 1;
		host->tls_dc_psk = zbx_hashset_insert(&config->psks, &psk_i_local, sizeof(ZBX_DC_PSK));
done:
		if (NULL != host->tls_dc_psk && NULL == psk_owner)
		{
			if (NULL == (psk_owner = zbx_hashset_search(&psk_owners, &host->tls_dc_psk->tls_psk_identity)))
			{
				/* register this host as the PSK identity owner, against which to report conflicts */

				psk_owner_local.first = (char *)host->tls_dc_psk->tls_psk_identity;
				psk_owner_local.second = (char *)host->host;

				zbx_hashset_insert(&psk_owners, &psk_owner_local, sizeof(psk_owner_local));
			}
		}
#endif
		ZBX_STR2UCHAR(host->tls_connect, row[29]);
		ZBX_STR2UCHAR(host->tls_accept, row[30]);

		if (0 == found)
		{
			host->maintenance_status = (unsigned char)atoi(row[7]);
			host->maintenance_type = (unsigned char)atoi(row[8]);
			host->maintenance_from = atoi(row[9]);
			host->data_expected_from = now;

			host->errors_from = atoi(row[10]);
			host->available = (unsigned char)atoi(row[11]);
			host->disable_until = atoi(row[12]);
			host->snmp_errors_from = atoi(row[13]);
			host->snmp_available = (unsigned char)atoi(row[14]);
			host->snmp_disable_until = atoi(row[15]);
			host->ipmi_errors_from = atoi(row[16]);
			host->ipmi_available = (unsigned char)atoi(row[17]);
			host->ipmi_disable_until = atoi(row[18]);
			host->jmx_errors_from = atoi(row[19]);
			host->jmx_available = (unsigned char)atoi(row[20]);
			host->jmx_disable_until = atoi(row[21]);
			host->availability_ts = now;

			DCstrpool_replace(0, &host->error, row[25]);
			DCstrpool_replace(0, &host->snmp_error, row[26]);
			DCstrpool_replace(0, &host->ipmi_error, row[27]);
			DCstrpool_replace(0, &host->jmx_error, row[28]);
		}
		else
		{
			if (HOST_STATUS_MONITORED == status && HOST_STATUS_MONITORED != host->status)
				host->data_expected_from = now;

			/* reset host status if host proxy assignment has been changed */
			if (proxy_hostid != host->proxy_hostid)
				host->used_interfaces = ZBX_FLAG_INTERFACE_UNKNOWN;
		}

		host->proxy_hostid = proxy_hostid;

		/* update 'hosts_h' and 'hosts_p' indexes using new data, if not done already */

		if (1 == update_index_h)
		{
			host_h_local.host = zbx_strpool_acquire(host->host);
			host_h_local.host_ptr = host;
			zbx_hashset_insert(&config->hosts_h, &host_h_local, sizeof(ZBX_DC_HOST_H));
		}

		if (1 == update_index_p)
		{
			host_p_local.host = zbx_strpool_acquire(host->host);
			host_p_local.host_ptr = host;
			zbx_hashset_insert(&config->hosts_p, &host_p_local, sizeof(ZBX_DC_HOST_H));
		}

		/* IPMI hosts */

		ipmi_authtype = (signed char)atoi(row[3]);
		ipmi_privilege = (unsigned char)atoi(row[4]);

		if (0 != ipmi_authtype || 2 != ipmi_privilege || '\0' != *row[5] || '\0' != *row[6])	/* useipmi */
		{
			ipmihost = DCfind_id(&config->ipmihosts, hostid, sizeof(ZBX_DC_IPMIHOST), &found);

			ipmihost->ipmi_authtype = ipmi_authtype;
			ipmihost->ipmi_privilege = ipmi_privilege;
			DCstrpool_replace(found, &ipmihost->ipmi_username, row[5]);
			DCstrpool_replace(found, &ipmihost->ipmi_password, row[6]);
		}
		else if (NULL != (ipmihost = zbx_hashset_search(&config->ipmihosts, &hostid)))
		{
			/* remove IPMI connection parameters for hosts without IPMI */

			zbx_strpool_release(ipmihost->ipmi_username);
			zbx_strpool_release(ipmihost->ipmi_password);

			zbx_hashset_remove_direct(&config->ipmihosts, ipmihost);
		}

		/* proxies */

		if (HOST_STATUS_PROXY_ACTIVE == status || HOST_STATUS_PROXY_PASSIVE == status)
		{
			proxy = DCfind_id(&config->proxies, hostid, sizeof(ZBX_DC_PROXY), &found);

			if (0 == found)
			{
				proxy->timediff = 0;
				proxy->location = ZBX_LOC_NOWHERE;
			}

			if (HOST_STATUS_PROXY_PASSIVE == status && (0 == found || status != host->status))
			{
				proxy->proxy_config_nextcheck = (int)calculate_proxy_nextcheck(
						hostid, CONFIG_PROXYCONFIG_FREQUENCY, now);
				proxy->proxy_data_nextcheck = (int)calculate_proxy_nextcheck(
						hostid, CONFIG_PROXYDATA_FREQUENCY, now);

				DCupdate_proxy_queue(proxy);
			}
			else if (HOST_STATUS_PROXY_ACTIVE == status && ZBX_LOC_QUEUE == proxy->location)
			{
				zbx_binary_heap_remove_direct(&config->pqueue, proxy->hostid);
				proxy->location = ZBX_LOC_NOWHERE;
			}
		}
		else if (NULL != (proxy = zbx_hashset_search(&config->proxies, &hostid)))
		{
			if (ZBX_LOC_QUEUE == proxy->location)
			{
				zbx_binary_heap_remove_direct(&config->pqueue, proxy->hostid);
				proxy->location = ZBX_LOC_NOWHERE;
			}

			zbx_hashset_remove_direct(&config->proxies, proxy);
		}

		host->status = status;
	}

	/* remove deleted hosts from buffer */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (host = zbx_hashset_search(&config->hosts, &rowid)))
			continue;

		hostid = host->hostid;

		/* IPMI hosts */

		if (NULL != (ipmihost = zbx_hashset_search(&config->ipmihosts, &hostid)))
		{
			zbx_strpool_release(ipmihost->ipmi_username);
			zbx_strpool_release(ipmihost->ipmi_password);

			zbx_hashset_remove_direct(&config->ipmihosts, ipmihost);
		}

		/* proxies */

		if (NULL != (proxy = zbx_hashset_search(&config->proxies, &hostid)))
		{
			if (ZBX_LOC_QUEUE == proxy->location)
			{
				zbx_binary_heap_remove_direct(&config->pqueue, proxy->hostid);
				proxy->location = ZBX_LOC_NOWHERE;
			}

			zbx_hashset_remove_direct(&config->proxies, proxy);
		}

		/* hosts */

		if (HOST_STATUS_MONITORED == host->status || HOST_STATUS_NOT_MONITORED == host->status)
		{
			host_h_local.host = host->host;
			host_h = zbx_hashset_search(&config->hosts_h, &host_h_local);

			if (NULL != host_h && host == host_h->host_ptr)	/* see ZBX-4045 for NULL check */
			{
				zbx_strpool_release(host_h->host);
				zbx_hashset_remove_direct(&config->hosts_h, host_h);
			}
		}
		else if (HOST_STATUS_PROXY_ACTIVE == host->status || HOST_STATUS_PROXY_PASSIVE == host->status)
		{
			host_p_local.host = host->host;
			host_p = zbx_hashset_search(&config->hosts_p, &host_p_local);

			if (NULL != host_p && host == host_p->host_ptr)
			{
				zbx_strpool_release(host_p->host);
				zbx_hashset_remove_direct(&config->hosts_p, host_p);
			}
		}

		zbx_strpool_release(host->host);
		zbx_strpool_release(host->name);
		zbx_strpool_release(host->error);
		zbx_strpool_release(host->snmp_error);
		zbx_strpool_release(host->ipmi_error);
		zbx_strpool_release(host->jmx_error);
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
		zbx_strpool_release(host->tls_issuer);
		zbx_strpool_release(host->tls_subject);

		/* Maintain 'psks' index. Unlink and delete the PSK identity. */
		if (NULL != host->tls_dc_psk)
		{
			psk_i_local.tls_psk_identity = host->tls_dc_psk->tls_psk_identity;

			if (NULL != (psk_i = zbx_hashset_search(&config->psks, &psk_i_local)) &&
					0 == --(psk_i->refcount))
			{
				zbx_strpool_release(psk_i->tls_psk_identity);
				zbx_strpool_release(psk_i->tls_psk);
				zbx_hashset_remove_direct(&config->psks, psk_i);
			}
		}
#endif
		zbx_hashset_remove_direct(&config->hosts, host);
	}

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	zbx_hashset_destroy(&psk_owners);
#endif

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void	DCsync_host_inventory(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_host_inventory";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_HOST_INVENTORY	*host_inventory;

	int			found, ret;
	zbx_uint64_t		hostid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(hostid, row[0]);

		host_inventory = DCfind_id(&config->host_inventories, hostid, sizeof(ZBX_DC_HOST_INVENTORY), &found);

		/* store new information in host_inventory structure */

		ZBX_STR2UCHAR(host_inventory->inventory_mode, row[1]);
	}

	/* remove deleted host inventory from cache */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (host_inventory = zbx_hashset_search(&config->host_inventories, &rowid)))
			continue;

		zbx_hashset_remove_direct(&config->host_inventories, host_inventory);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void	DCsync_htmpls(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_htmpls";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_HTMPL		*htmpl = NULL;

	int			found, i, index, ret;
	zbx_uint64_t		_hostid = 0, hostid, templateid;
	zbx_vector_ptr_t	sort;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_ptr_create(&sort);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(hostid, row[0]);
		ZBX_STR2UINT64(templateid, row[1]);

		if (_hostid != hostid || 0 == _hostid)
		{
			_hostid = hostid;

			htmpl = DCfind_id(&config->htmpls, hostid, sizeof(ZBX_DC_HTMPL), &found);

			if (0 == found)
			{
				zbx_vector_uint64_create_ext(&htmpl->templateids,
						__config_mem_malloc_func,
						__config_mem_realloc_func,
						__config_mem_free_func);
				zbx_vector_uint64_reserve(&htmpl->templateids, 1);
			}

			/* host template ids with added rows need to be sorted */
			zbx_vector_ptr_append(&sort, htmpl);
		}

		zbx_vector_uint64_append(&htmpl->templateids, templateid);
	}

	/* remove deleted host templates from cache */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		ZBX_STR2UINT64(hostid, row[0]);

		if (NULL == (htmpl = (ZBX_DC_HTMPL *)zbx_hashset_search(&config->htmpls, &hostid)))
			continue;

		ZBX_STR2UINT64(templateid, row[1]);

		if (-1 == (index = zbx_vector_uint64_search(&htmpl->templateids, templateid,
				ZBX_DEFAULT_UINT64_COMPARE_FUNC)))
		{
			continue;
		}

		if (1 == htmpl->templateids.values_num)
		{
			zbx_vector_uint64_destroy(&htmpl->templateids);
			zbx_hashset_remove_direct(&config->htmpls, htmpl);
		}
		else
		{
			zbx_vector_uint64_remove_noorder(&htmpl->templateids, index);
			zbx_vector_ptr_append(&sort, htmpl);
		}
	}

	zbx_vector_ptr_sort(&sort, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);
	zbx_vector_ptr_uniq(&sort, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

	/* sort the template lists with new rows */
	for (i = 0; i < sort.values_num; i++)
	{
		htmpl = (ZBX_DC_HTMPL *)sort.values[i];
		zbx_vector_uint64_sort(&htmpl->templateids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	}

	zbx_vector_ptr_destroy(&sort);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void	DCsync_gmacros(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_gmacros";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_GMACRO		*gmacro;

	int			found, context_existed, update_index, ret;
	zbx_uint64_t		globalmacroid;
	char			*macro = NULL, *context = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(globalmacroid, row[0]);

		if (SUCCEED != zbx_user_macro_parse_dyn(row[1], &macro, &context, NULL))
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot parse user macro \"%s\"", row[1]);
			continue;
		}

		gmacro = DCfind_id(&config->gmacros, globalmacroid, sizeof(ZBX_DC_GMACRO), &found);

		/* see whether we should and can update gmacros_m index at this point */
		update_index = 0;

		if (0 == found || 0 != strcmp(gmacro->macro, macro) || 0 != zbx_strcmp_null(gmacro->context, context))
		{
			if (1 == found)
				config_gmacro_remove_index(&config->gmacros_m, gmacro);

			update_index = 1;
		}

		/* store new information in macro structure */
		DCstrpool_replace(found, &gmacro->macro, macro);
		DCstrpool_replace(found, &gmacro->value, row[2]);

		context_existed = (1 == found && NULL != gmacro->context);

		if (NULL == context)
		{
			/* release the context if it was removed from the macro */
			if (1 == context_existed)
				zbx_strpool_release(gmacro->context);

			gmacro->context = NULL;
		}
		else
		{
			/* replace the existing context (1) or add context to macro (0) */
			DCstrpool_replace(context_existed, &gmacro->context, context);
		}

		/* update gmacros_m index using new data */
		if (1 == update_index)
			config_gmacro_add_index(&config->gmacros_m, gmacro);
	}

	/* remove deleted globalmacros from buffer */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (gmacro = zbx_hashset_search(&config->gmacros, &rowid)))
			continue;

		config_gmacro_remove_index(&config->gmacros_m, gmacro);

		zbx_strpool_release(gmacro->macro);
		zbx_strpool_release(gmacro->value);

		if (NULL != gmacro->context)
			zbx_strpool_release(gmacro->context);

		zbx_hashset_remove_direct(&config->gmacros, gmacro);
	}

	zbx_free(context);
	zbx_free(macro);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void	DCsync_hmacros(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_hmacros";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_HMACRO		*hmacro;

	int			found, context_existed, update_index, ret;
	zbx_uint64_t		hostmacroid, hostid;
	char			*macro = NULL, *context = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(hostmacroid, row[0]);
		ZBX_STR2UINT64(hostid, row[1]);

		if (SUCCEED != zbx_user_macro_parse_dyn(row[2], &macro, &context, NULL))
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot parse host \"%s\" macro \"%s\"", row[1], row[2]);
			continue;
		}

		hmacro = DCfind_id(&config->hmacros, hostmacroid, sizeof(ZBX_DC_HMACRO), &found);

		/* see whether we should and can update hmacros_hm index at this point */
		update_index = 0;

		if (0 == found || hmacro->hostid != hostid || 0 != strcmp(hmacro->macro, macro) ||
				0 != zbx_strcmp_null(hmacro->context, context))
		{
			if (1 == found)
				config_hmacro_remove_index(&config->hmacros_hm, hmacro);

			update_index = 1;
		}

		/* store new information in macro structure */
		hmacro->hostid = hostid;
		DCstrpool_replace(found, &hmacro->macro, macro);
		DCstrpool_replace(found, &hmacro->value, row[3]);

		context_existed = (1 == found && NULL != hmacro->context);

		if (NULL == context)
		{
			/* release the context if it was removed from the macro */
			if (1 == context_existed)
				zbx_strpool_release(hmacro->context);

			hmacro->context = NULL;
		}
		else
		{
			/* replace the existing context (1) or add context to macro (0) */
			DCstrpool_replace(context_existed, &hmacro->context, context);
		}

		/* update hmacros_hm index using new data */
		if (1 == update_index)
			config_hmacro_add_index(&config->hmacros_hm, hmacro);
	}

	/* remove deleted host macros from buffer */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (hmacro = zbx_hashset_search(&config->hmacros, &rowid)))
			continue;

		config_hmacro_remove_index(&config->hmacros_hm, hmacro);

		zbx_strpool_release(hmacro->macro);
		zbx_strpool_release(hmacro->value);

		if (NULL != hmacro->context)
			zbx_strpool_release(hmacro->context);

		zbx_hashset_remove_direct(&config->hmacros, hmacro);
	}

	zbx_free(context);
	zbx_free(macro);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: substitute_host_interface_macros                                 *
 *                                                                            *
 * Purpose: trying to resolve the macros in host inteface                     *
 *                                                                            *
 ******************************************************************************/
static void	substitute_host_interface_macros(ZBX_DC_INTERFACE *interface)
{
	int	macros;
	char	*addr;
	DC_HOST	host;

	macros = STR_CONTAINS_MACROS(interface->ip) ? 0x01 : 0;
	macros |= STR_CONTAINS_MACROS(interface->dns) ? 0x02 : 0;

	if (0 != macros)
	{
		DCget_host_by_hostid(&host, interface->hostid);

		if (0 != (macros & 0x01))
		{
			addr = zbx_strdup(NULL, interface->ip);
			substitute_simple_macros(NULL, NULL, NULL, NULL, NULL, &host, NULL, NULL,
					&addr, MACRO_TYPE_INTERFACE_ADDR, NULL, 0);
			DCstrpool_replace(1, &interface->ip, addr);
			zbx_free(addr);
		}

		if (0 != (macros & 0x02))
		{
			addr = zbx_strdup(NULL, interface->dns);
			substitute_simple_macros(NULL, NULL, NULL, NULL, NULL, &host, NULL, NULL,
					&addr, MACRO_TYPE_INTERFACE_ADDR, NULL, 0);
			DCstrpool_replace(1, &interface->dns, addr);
			zbx_free(addr);
		}
	}
}

/******************************************************************************
 *                                                                            *
 * Function: dc_interface_snmpaddrs_remove                                    *
 *                                                                            *
 * Purpose: remove interface from SNMP address -> interfaceid index           *
 *                                                                            *
 * Parameters: interface - [IN] the interface                                 *
 *                                                                            *
 ******************************************************************************/
static void	dc_interface_snmpaddrs_remove(ZBX_DC_INTERFACE *interface)
{
	ZBX_DC_INTERFACE_ADDR	*ifaddr, ifaddr_local;
	int			index;

	ifaddr_local.addr = (0 != interface->useip ? interface->ip : interface->dns);

	if ('\0' == *ifaddr_local.addr)
		return;

	if (NULL == (ifaddr = (ZBX_DC_INTERFACE_ADDR *)zbx_hashset_search(&config->interface_snmpaddrs, &ifaddr_local)))
		return;

	if (FAIL == (index = zbx_vector_uint64_search(&ifaddr->interfaceids, interface->interfaceid,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC)))
	{
		return;
	}

	zbx_vector_uint64_remove_noorder(&ifaddr->interfaceids, index);

	if (0 == ifaddr->interfaceids.values_num)
	{
		zbx_strpool_release(ifaddr->addr);
		zbx_vector_uint64_destroy(&ifaddr->interfaceids);
		zbx_hashset_remove_direct(&config->interface_snmpaddrs, ifaddr);
	}
}

static void	DCsync_interfaces(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_interfaces";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_INTERFACE	*interface;
	ZBX_DC_INTERFACE_HT	*interface_ht, interface_ht_local;
	ZBX_DC_INTERFACE_ADDR	*interface_snmpaddr, interface_snmpaddr_local;

	int			found, update_index, ret, i;
	zbx_uint64_t		interfaceid, hostid;
	unsigned char		type, main_, useip;
	unsigned char		bulk, reset_snmp_stats;
	zbx_vector_ptr_t	interfaces;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_ptr_create(&interfaces);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(interfaceid, row[0]);
		ZBX_STR2UINT64(hostid, row[1]);
		ZBX_STR2UCHAR(type, row[2]);
		ZBX_STR2UCHAR(main_, row[3]);
		ZBX_STR2UCHAR(useip, row[4]);
		ZBX_STR2UCHAR(bulk, row[8]);

		interface = DCfind_id(&config->interfaces, interfaceid, sizeof(ZBX_DC_INTERFACE), &found);
		zbx_vector_ptr_append(&interfaces, interface);

		/* remove old address->interfaceid index */
		if (0 != found && INTERFACE_TYPE_SNMP == interface->type)
			dc_interface_snmpaddrs_remove(interface);

		/* see whether we should and can update interfaces_ht index at this point */

		update_index = 0;

		if (0 == found || interface->hostid != hostid || interface->type != type || interface->main != main_)
		{
			if (1 == found && 1 == interface->main)
			{
				interface_ht_local.hostid = interface->hostid;
				interface_ht_local.type = interface->type;
				interface_ht = zbx_hashset_search(&config->interfaces_ht, &interface_ht_local);

				if (NULL != interface_ht && interface == interface_ht->interface_ptr)
				{
					/* see ZBX-4045 for NULL check in the conditional */
					zbx_hashset_remove(&config->interfaces_ht, &interface_ht_local);
				}
			}

			if (1 == main_)
			{
				interface_ht_local.hostid = hostid;
				interface_ht_local.type = type;
				interface_ht = zbx_hashset_search(&config->interfaces_ht, &interface_ht_local);

				if (NULL != interface_ht)
					interface_ht->interface_ptr = interface;
				else
					update_index = 1;
			}
		}

		/* store new information in interface structure */

		reset_snmp_stats = (0 == found || interface->hostid != hostid || interface->type != type ||
						interface->useip != useip || interface->bulk != bulk);

		interface->hostid = hostid;
		interface->type = type;
		interface->main = main_;
		interface->useip = useip;
		interface->bulk = bulk;
		reset_snmp_stats |= (SUCCEED == DCstrpool_replace(found, &interface->ip, row[5]));
		reset_snmp_stats |= (SUCCEED == DCstrpool_replace(found, &interface->dns, row[6]));
		reset_snmp_stats |= (SUCCEED == DCstrpool_replace(found, &interface->port, row[7]));

		/* update interfaces_ht index using new data, if not done already */

		if (1 == update_index)
		{
			interface_ht_local.hostid = interface->hostid;
			interface_ht_local.type = interface->type;
			interface_ht_local.interface_ptr = interface;
			zbx_hashset_insert(&config->interfaces_ht, &interface_ht_local, sizeof(ZBX_DC_INTERFACE_HT));
		}

		/* update interface_snmpaddrs for SNMP traps or reset bulk request statistics */

		if (INTERFACE_TYPE_SNMP == interface->type)
		{
			interface_snmpaddr_local.addr = (0 != interface->useip ? interface->ip : interface->dns);

			if ('\0' != *interface_snmpaddr_local.addr)
			{
				if (NULL == (interface_snmpaddr = zbx_hashset_search(&config->interface_snmpaddrs,
						&interface_snmpaddr_local)))
				{
					zbx_strpool_acquire(interface_snmpaddr_local.addr);

					interface_snmpaddr = zbx_hashset_insert(&config->interface_snmpaddrs,
							&interface_snmpaddr_local, sizeof(ZBX_DC_INTERFACE_ADDR));
					zbx_vector_uint64_create_ext(&interface_snmpaddr->interfaceids,
							__config_mem_malloc_func,
							__config_mem_realloc_func,
							__config_mem_free_func);
				}

				zbx_vector_uint64_append(&interface_snmpaddr->interfaceids, interfaceid);
			}

			if (1 == reset_snmp_stats)
			{
				interface->max_snmp_succeed = 0;
				interface->min_snmp_fail = MAX_SNMP_ITEMS + 1;
			}
		}

		/* first resolve macros for ip and dns fields in main agent interface  */
		/* because other interfaces might reference main interfaces ip and dns */
		/* with {HOST.IP} and {HOST.DNS} macros                                */
		if (1 == interface->main && INTERFACE_TYPE_AGENT == interface->type)
			substitute_host_interface_macros(interface);
	}

	/* resolve macros in other interfaces */

	for (i = 0; i < interfaces.values_num; i++)
	{
		interface = (ZBX_DC_INTERFACE *)interfaces.values[i];

		if (1 != interface->main || INTERFACE_TYPE_AGENT != interface->type)
			substitute_host_interface_macros(interface);
	}

	/* remove deleted interfaces from buffer */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (interface = zbx_hashset_search(&config->interfaces, &rowid)))
			continue;

		if (INTERFACE_TYPE_SNMP == interface->type)
			dc_interface_snmpaddrs_remove(interface);

		if (1 == interface->main)
		{
			interface_ht_local.hostid = interface->hostid;
			interface_ht_local.type = interface->type;
			interface_ht = zbx_hashset_search(&config->interfaces_ht, &interface_ht_local);

			if (NULL != interface_ht && interface == interface_ht->interface_ptr)
			{
				/* see ZBX-4045 for NULL check in the conditional */
				zbx_hashset_remove(&config->interfaces_ht, &interface_ht_local);
			}
		}

		zbx_strpool_release(interface->ip);
		zbx_strpool_release(interface->dns);
		zbx_strpool_release(interface->port);

		zbx_hashset_remove_direct(&config->interfaces, interface);
	}

	zbx_vector_ptr_destroy(&interfaces);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_interface_snmpitems_remove                                    *
 *                                                                            *
 * Purpose: remove item from interfaceid -> itemid index                      *
 *                                                                            *
 * Parameters: interface - [IN] the item                                      *
 *                                                                            *
 ******************************************************************************/
static void	dc_interface_snmpitems_remove(ZBX_DC_ITEM *item)
{
	ZBX_DC_INTERFACE_ITEM	*ifitem;
	int			index;
	zbx_uint64_t		interfaceid;

	if (0 == (interfaceid = item->interfaceid))
		return;

	if (NULL == (ifitem = (ZBX_DC_INTERFACE_ITEM *)zbx_hashset_search(&config->interface_snmpitems, &interfaceid)))
		return;

	if (FAIL == (index = zbx_vector_uint64_search(&ifitem->itemids, item->itemid, ZBX_DEFAULT_UINT64_COMPARE_FUNC)))
		return

	zbx_vector_uint64_remove_noorder(&ifitem->itemids, index);

	if (0 == ifitem->itemids.values_num)
	{
		zbx_vector_uint64_destroy(&ifitem->itemids);
		zbx_hashset_remove_direct(&config->interface_snmpitems, ifitem);
	}
}

static void	DCsync_items(zbx_dbsync_t *sync, int refresh_unsupported_changed)
{
	const char		*__function_name = "DCsync_items";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_HOST		*host;

	ZBX_DC_ITEM		*item;
	ZBX_DC_NUMITEM		*numitem;
	ZBX_DC_SNMPITEM		*snmpitem;
	ZBX_DC_IPMIITEM		*ipmiitem;
	ZBX_DC_FLEXITEM		*flexitem;
	ZBX_DC_TRAPITEM		*trapitem;
	ZBX_DC_LOGITEM		*logitem;
	ZBX_DC_DBITEM		*dbitem;
	ZBX_DC_SSHITEM		*sshitem;
	ZBX_DC_TELNETITEM	*telnetitem;
	ZBX_DC_SIMPLEITEM	*simpleitem;
	ZBX_DC_JMXITEM		*jmxitem;
	ZBX_DC_CALCITEM		*calcitem;
	ZBX_DC_INTERFACE_ITEM	*interface_snmpitem;
	ZBX_DC_ITEM_HK		*item_hk, item_hk_local;
	ZBX_DC_DELTAITEM	*deltaitem;

	time_t			now;
	unsigned char		old_poller_type, status, type;
	int			old_nextcheck, delay, delay_flex_changed, key_changed, found, update_index, ret;
	zbx_uint64_t		itemid, hostid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	now = time(NULL);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(itemid, row[0]);
		ZBX_STR2UINT64(hostid, row[1]);
		ZBX_STR2UCHAR(status, row[2]);
		ZBX_STR2UCHAR(type, row[3]);

		if (NULL == (host = zbx_hashset_search(&config->hosts, &hostid)))
			continue;

		item = DCfind_id(&config->items, itemid, sizeof(ZBX_DC_ITEM), &found);

		if (0 != found && ITEM_TYPE_SNMPTRAP == item->type)
			dc_interface_snmpitems_remove(item);

		/* see whether we should and can update items_hk index at this point */

		update_index = 0;

		if (0 == found || item->hostid != hostid || 0 != strcmp(item->key, row[6]))
		{
			if (1 == found)
			{
				item_hk_local.hostid = item->hostid;
				item_hk_local.key = item->key;
				item_hk = zbx_hashset_search(&config->items_hk, &item_hk_local);

				if (item == item_hk->item_ptr)
				{
					zbx_strpool_release(item_hk->key);
					zbx_hashset_remove_direct(&config->items_hk, item_hk);
				}
			}

			item_hk_local.hostid = hostid;
			item_hk_local.key = row[6];
			item_hk = zbx_hashset_search(&config->items_hk, &item_hk_local);

			if (NULL != item_hk)
				item_hk->item_ptr = item;
			else
				update_index = 1;
		}

		/* store new information in item structure */

		item->hostid = hostid;
		item->data_type = (unsigned char)atoi(row[4]);
		DCstrpool_replace(found, &item->port, row[9]);
		item->flags = (unsigned char)atoi(row[26]);
		ZBX_DBROW2UINT64(item->interfaceid, row[27]);
		if (ZBX_HK_OPTION_ENABLED == config->config->hk.history_global)
			item->history = config->config->hk.history;
		else
			item->history = atoi(row[36]);
		ZBX_STR2UCHAR(item->inventory_link, row[38]);
		ZBX_DBROW2UINT64(item->valuemapid, row[39]);

		if (0 != (ZBX_FLAG_DISCOVERY_RULE & item->flags))
			item->value_type = ITEM_VALUE_TYPE_TEXT;
		else
			item->value_type = (unsigned char)atoi(row[5]);

		key_changed = (SUCCEED == DCstrpool_replace(found, &item->key, row[6]));

		if (0 == found)
		{
			item->triggers = NULL;
			item->update_triggers = 1;
			item->nextcheck = 0;
			item->lastclock = 0;
			item->state = (unsigned char)atoi(row[20]);
			item->db_state = item->state;
			ZBX_STR2UINT64(item->lastlogsize, row[31]);
			item->mtime = atoi(row[32]);
			DCstrpool_replace(found, &item->db_error, row[41]);
			item->data_expected_from = now;
			item->location = ZBX_LOC_NOWHERE;
			old_poller_type = ZBX_NO_POLLER;
			item->unreachable = 0;
		}
		else
		{
			if (ITEM_STATUS_ACTIVE == status && ITEM_STATUS_ACTIVE != item->status)
				item->data_expected_from = now;

			old_poller_type = item->poller_type;
		}

		item->status = status;
		old_nextcheck = item->nextcheck;

		/* update items_hk index using new data, if not done already */

		if (1 == update_index)
		{
			item_hk_local.hostid = item->hostid;
			item_hk_local.key = zbx_strpool_acquire(item->key);
			item_hk_local.item_ptr = item;
			zbx_hashset_insert(&config->items_hk, &item_hk_local, sizeof(ZBX_DC_ITEM_HK));
		}

		/* update item nextcheck and process items with flexible intervals */

		delay = atoi(row[15]);

		if ('\0' != *row[16])
		{
			int	found1;

			flexitem = DCfind_id(&config->flexitems, itemid, sizeof(ZBX_DC_FLEXITEM), &found1);

			delay_flex_changed = (SUCCEED == DCstrpool_replace(found1, &flexitem->delay_flex, row[16]));
		}
		else
		{
			if (NULL != (flexitem = zbx_hashset_search(&config->flexitems, &itemid)))
			{
				/* remove delay_flex parameter for non-flexible item */

				zbx_strpool_release(flexitem->delay_flex);
				zbx_hashset_remove_direct(&config->flexitems, flexitem);

				flexitem = NULL;

				delay_flex_changed = 1;
			}
			else
				delay_flex_changed = 0;
		}

		if (ITEM_STATUS_ACTIVE == item->status && HOST_STATUS_MONITORED == host->status)
		{
			item->poller_type = poller_by_item(host->proxy_hostid, type, item->key);

			if (ZBX_POLLER_TYPE_UNREACHABLE == old_poller_type &&
					(ZBX_POLLER_TYPE_NORMAL == item->poller_type ||
					ZBX_POLLER_TYPE_IPMI == item->poller_type ||
					ZBX_POLLER_TYPE_JAVA == item->poller_type))
			{
				item->poller_type = ZBX_POLLER_TYPE_UNREACHABLE;
			}

			if (SUCCEED == is_counted_in_item_queue(type, item->key) &&
					(0 == item->nextcheck || 0 != key_changed || item->type != type ||
					(ITEM_STATE_NOTSUPPORTED == item->state && 1 == refresh_unsupported_changed) ||
					(ITEM_STATE_NORMAL == item->state &&
					(item->delay != delay || 0 != delay_flex_changed))))
			{
				ZBX_DC_PROXY	*proxy = NULL;
				zbx_uint64_t	seed;
				int		proxy_timediff;

				if (0 != host->proxy_hostid)
					proxy = zbx_hashset_search(&config->proxies, &host->proxy_hostid);

				proxy_timediff = (NULL == proxy ? 0 : proxy->timediff);

				seed = get_item_nextcheck_seed(item->itemid, item->interfaceid, type, item->key);

				if (ITEM_STATE_NOTSUPPORTED == item->state)
				{
					item->nextcheck = calculate_item_nextcheck(seed, type,
							config->config->refresh_unsupported, NULL,
							now - proxy_timediff);
					item->nextcheck += proxy_timediff + (NULL != proxy);
				}
				else
				{
					item->nextcheck = calculate_item_nextcheck(seed, type, delay,
							(NULL == flexitem ? NULL : row[16]), now - proxy_timediff);
					item->nextcheck += proxy_timediff + (NULL != proxy);
				}
			}

			/* update host's used_interfaces flag */
			if (ZBX_FLAG_INTERFACE_UNKNOWN != host->used_interfaces)
			{
				switch (type)
				{
					case ITEM_TYPE_ZABBIX:
						host->used_interfaces |= ZBX_FLAG_INTERFACE_ZABBIX;
						break;
					case ITEM_TYPE_SNMPv1:
					case ITEM_TYPE_SNMPv2c:
					case ITEM_TYPE_SNMPv3:
						host->used_interfaces |= ZBX_FLAG_INTERFACE_SNMP;
						break;
					case ITEM_TYPE_IPMI:
						host->used_interfaces |= ZBX_FLAG_INTERFACE_IPMI;
						break;
					case ITEM_TYPE_JMX:
						host->used_interfaces |= ZBX_FLAG_INTERFACE_JMX;
						break;
				}
			}
		}
		else
		{
			item->poller_type = ZBX_NO_POLLER;
			item->nextcheck = 0;
			item->unreachable = 0;
		}

		item->delay = delay;
		item->type = type;

		/* numeric items */

		if (ITEM_VALUE_TYPE_FLOAT == item->value_type || ITEM_VALUE_TYPE_UINT64 == item->value_type)
		{
			numitem = DCfind_id(&config->numitems, itemid, sizeof(ZBX_DC_NUMITEM), &found);

			ZBX_STR2UCHAR(numitem->delta, row[33]);
			ZBX_STR2UCHAR(numitem->multiplier, row[34]);
			DCstrpool_replace(found, &numitem->formula, row[35]);
			if (ZBX_HK_OPTION_ENABLED == config->config->hk.trends_global)
				numitem->trends = config->config->hk.trends;
			else
				numitem->trends = atoi(row[37]);
			DCstrpool_replace(found, &numitem->units, row[40]);
		}
		else if (NULL != (numitem = zbx_hashset_search(&config->numitems, &itemid)))
		{
			/* remove parameters for non-numeric item */

			zbx_strpool_release(numitem->formula);
			zbx_strpool_release(numitem->units);

			zbx_hashset_remove_direct(&config->numitems, numitem);
		}

		/* SNMP items */

		if (SUCCEED == is_snmp_type(item->type))
		{
			snmpitem = DCfind_id(&config->snmpitems, itemid, sizeof(ZBX_DC_SNMPITEM), &found);

			DCstrpool_replace(found, &snmpitem->snmp_community, row[7]);
			DCstrpool_replace(found, &snmpitem->snmpv3_securityname, row[10]);
			snmpitem->snmpv3_securitylevel = (unsigned char)atoi(row[11]);
			DCstrpool_replace(found, &snmpitem->snmpv3_authpassphrase, row[12]);
			DCstrpool_replace(found, &snmpitem->snmpv3_privpassphrase, row[13]);
			snmpitem->snmpv3_authprotocol = (unsigned char)atoi(row[28]);
			snmpitem->snmpv3_privprotocol = (unsigned char)atoi(row[29]);
			DCstrpool_replace(found, &snmpitem->snmpv3_contextname, row[30]);

			if (SUCCEED == DCstrpool_replace(found, &snmpitem->snmp_oid, row[8]))
			{
				if (NULL != strchr(snmpitem->snmp_oid, '{'))
					snmpitem->snmp_oid_type = ZBX_SNMP_OID_TYPE_MACRO;
				else if (NULL != strchr(snmpitem->snmp_oid, '['))
					snmpitem->snmp_oid_type = ZBX_SNMP_OID_TYPE_DYNAMIC;
				else
					snmpitem->snmp_oid_type = ZBX_SNMP_OID_TYPE_NORMAL;
			}
		}
		else if (NULL != (snmpitem = zbx_hashset_search(&config->snmpitems, &itemid)))
		{
			/* remove SNMP parameters for non-SNMP item */

			zbx_strpool_release(snmpitem->snmp_community);
			zbx_strpool_release(snmpitem->snmp_oid);
			zbx_strpool_release(snmpitem->snmpv3_securityname);
			zbx_strpool_release(snmpitem->snmpv3_authpassphrase);
			zbx_strpool_release(snmpitem->snmpv3_privpassphrase);
			zbx_strpool_release(snmpitem->snmpv3_contextname);

			zbx_hashset_remove_direct(&config->snmpitems, snmpitem);
		}

		/* IPMI items */

		if (ITEM_TYPE_IPMI == item->type)
		{
			ipmiitem = DCfind_id(&config->ipmiitems, itemid, sizeof(ZBX_DC_IPMIITEM), &found);

			DCstrpool_replace(found, &ipmiitem->ipmi_sensor, row[14]);
		}
		else if (NULL != (ipmiitem = zbx_hashset_search(&config->ipmiitems, &itemid)))
		{
			/* remove IPMI parameters for non-IPMI item */
			zbx_strpool_release(ipmiitem->ipmi_sensor);
			zbx_hashset_remove_direct(&config->ipmiitems, ipmiitem);
		}

		/* trapper items */

		if (ITEM_TYPE_TRAPPER == item->type && '\0' != *row[17])
		{
			trapitem = DCfind_id(&config->trapitems, itemid, sizeof(ZBX_DC_TRAPITEM), &found);
			zbx_trim_str_list(row[17], ',');
			DCstrpool_replace(found, &trapitem->trapper_hosts, row[17]);
		}
		else if (NULL != (trapitem = zbx_hashset_search(&config->trapitems, &itemid)))
		{
			/* remove trapper_hosts parameter */
			zbx_strpool_release(trapitem->trapper_hosts);
			zbx_hashset_remove_direct(&config->trapitems, trapitem);
		}

		/* log items */

		if (ITEM_VALUE_TYPE_LOG == item->value_type && '\0' != *row[18])
		{
			logitem = DCfind_id(&config->logitems, itemid, sizeof(ZBX_DC_LOGITEM), &found);

			DCstrpool_replace(found, &logitem->logtimefmt, row[18]);
		}
		else if (NULL != (logitem = zbx_hashset_search(&config->logitems, &itemid)))
		{
			/* remove logtimefmt parameter */
			zbx_strpool_release(logitem->logtimefmt);
			zbx_hashset_remove_direct(&config->logitems, logitem);
		}

		/* db items */

		if (ITEM_TYPE_DB_MONITOR == item->type && '\0' != *row[19])
		{
			dbitem = DCfind_id(&config->dbitems, itemid, sizeof(ZBX_DC_DBITEM), &found);

			DCstrpool_replace(found, &dbitem->params, row[19]);
			DCstrpool_replace(found, &dbitem->username, row[22]);
			DCstrpool_replace(found, &dbitem->password, row[23]);
		}
		else if (NULL != (dbitem = zbx_hashset_search(&config->dbitems, &itemid)))
		{
			/* remove db item parameters */
			zbx_strpool_release(dbitem->params);
			zbx_strpool_release(dbitem->username);
			zbx_strpool_release(dbitem->password);

			zbx_hashset_remove_direct(&config->dbitems, dbitem);
		}

		/* SSH items */

		if (ITEM_TYPE_SSH == item->type)
		{
			sshitem = DCfind_id(&config->sshitems, itemid, sizeof(ZBX_DC_SSHITEM), &found);

			sshitem->authtype = (unsigned char)atoi(row[21]);
			DCstrpool_replace(found, &sshitem->username, row[22]);
			DCstrpool_replace(found, &sshitem->password, row[23]);
			DCstrpool_replace(found, &sshitem->publickey, row[24]);
			DCstrpool_replace(found, &sshitem->privatekey, row[25]);
			DCstrpool_replace(found, &sshitem->params, row[19]);
		}
		else if (NULL != (sshitem = zbx_hashset_search(&config->sshitems, &itemid)))
		{
			/* remove SSH item parameters */

			zbx_strpool_release(sshitem->username);
			zbx_strpool_release(sshitem->password);
			zbx_strpool_release(sshitem->publickey);
			zbx_strpool_release(sshitem->privatekey);
			zbx_strpool_release(sshitem->params);

			zbx_hashset_remove_direct(&config->sshitems, sshitem);
		}

		/* TELNET items */

		if (ITEM_TYPE_TELNET == item->type)
		{
			telnetitem = DCfind_id(&config->telnetitems, itemid, sizeof(ZBX_DC_TELNETITEM), &found);

			DCstrpool_replace(found, &telnetitem->username, row[22]);
			DCstrpool_replace(found, &telnetitem->password, row[23]);
			DCstrpool_replace(found, &telnetitem->params, row[19]);
		}
		else if (NULL != (telnetitem = zbx_hashset_search(&config->telnetitems, &itemid)))
		{
			/* remove TELNET item parameters */

			zbx_strpool_release(telnetitem->username);
			zbx_strpool_release(telnetitem->password);
			zbx_strpool_release(telnetitem->params);

			zbx_hashset_remove_direct(&config->telnetitems, telnetitem);
		}

		/* simple items */

		if (ITEM_TYPE_SIMPLE == item->type)
		{
			simpleitem = DCfind_id(&config->simpleitems, itemid, sizeof(ZBX_DC_SIMPLEITEM), &found);

			DCstrpool_replace(found, &simpleitem->username, row[22]);
			DCstrpool_replace(found, &simpleitem->password, row[23]);
		}
		else if (NULL != (simpleitem = zbx_hashset_search(&config->simpleitems, &itemid)))
		{
			/* remove simple item parameters */

			zbx_strpool_release(simpleitem->username);
			zbx_strpool_release(simpleitem->password);

			zbx_hashset_remove_direct(&config->simpleitems, simpleitem);
		}

		/* JMX items */

		if (ITEM_TYPE_JMX == item->type)
		{
			jmxitem = DCfind_id(&config->jmxitems, itemid, sizeof(ZBX_DC_JMXITEM), &found);

			DCstrpool_replace(found, &jmxitem->username, row[22]);
			DCstrpool_replace(found, &jmxitem->password, row[23]);
		}
		else if (NULL != (jmxitem = zbx_hashset_search(&config->jmxitems, &itemid)))
		{
			/* remove JMX item parameters */

			zbx_strpool_release(jmxitem->username);
			zbx_strpool_release(jmxitem->password);

			zbx_hashset_remove_direct(&config->jmxitems, jmxitem);
		}

		/* SNMP trap items for current server/proxy */

		if (ITEM_TYPE_SNMPTRAP == item->type && 0 == host->proxy_hostid)
		{
			interface_snmpitem = DCfind_id(&config->interface_snmpitems,
					item->interfaceid, sizeof(ZBX_DC_INTERFACE_ITEM), &found);

			if (0 == found)
			{
				zbx_vector_uint64_create_ext(&interface_snmpitem->itemids,
						__config_mem_malloc_func,
						__config_mem_realloc_func,
						__config_mem_free_func);
			}

			zbx_vector_uint64_append(&interface_snmpitem->itemids, itemid);
		}

		/* calculated items */

		if (ITEM_TYPE_CALCULATED == item->type)
		{
			calcitem = DCfind_id(&config->calcitems, itemid, sizeof(ZBX_DC_CALCITEM), &found);

			DCstrpool_replace(found, &calcitem->params, row[19]);
		}
		else if (NULL != (calcitem = zbx_hashset_search(&config->calcitems, &itemid)))
		{
			/* remove calculated item parameters */

			zbx_strpool_release(calcitem->params);
			zbx_hashset_remove_direct(&config->calcitems, calcitem);
		}

		DCupdate_item_queue(item, old_poller_type, old_nextcheck);
	}

	/* remove deleted items from buffer */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (item = zbx_hashset_search(&config->items, &rowid)))
			continue;

		itemid = item->itemid;

		if (ITEM_TYPE_SNMPTRAP == item->type)
			dc_interface_snmpitems_remove(item);

		/* numeric items */

		if (ITEM_VALUE_TYPE_FLOAT == item->value_type || ITEM_VALUE_TYPE_UINT64 == item->value_type)
		{
			numitem = zbx_hashset_search(&config->numitems, &itemid);

			zbx_strpool_release(numitem->formula);
			zbx_strpool_release(numitem->units);

			zbx_hashset_remove_direct(&config->numitems, numitem);
		}

		/* SNMP items */

		if (SUCCEED == is_snmp_type(item->type))
		{
			snmpitem = zbx_hashset_search(&config->snmpitems, &itemid);

			zbx_strpool_release(snmpitem->snmp_community);
			zbx_strpool_release(snmpitem->snmp_oid);
			zbx_strpool_release(snmpitem->snmpv3_securityname);
			zbx_strpool_release(snmpitem->snmpv3_authpassphrase);
			zbx_strpool_release(snmpitem->snmpv3_privpassphrase);
			zbx_strpool_release(snmpitem->snmpv3_contextname);

			zbx_hashset_remove_direct(&config->snmpitems, snmpitem);
		}

		/* IPMI items */

		if (ITEM_TYPE_IPMI == item->type)
		{
			ipmiitem = zbx_hashset_search(&config->ipmiitems, &itemid);
			zbx_strpool_release(ipmiitem->ipmi_sensor);
			zbx_hashset_remove_direct(&config->ipmiitems, ipmiitem);
		}

		/* items with flexible intervals */

		if (NULL != (flexitem = zbx_hashset_search(&config->flexitems, &itemid)))
		{
			zbx_strpool_release(flexitem->delay_flex);
			zbx_hashset_remove_direct(&config->flexitems, flexitem);
		}

		/* trapper items */

		if (ITEM_TYPE_TRAPPER == item->type &&
				NULL != (trapitem = zbx_hashset_search(&config->trapitems, &itemid)))
		{
			zbx_strpool_release(trapitem->trapper_hosts);
			zbx_hashset_remove_direct(&config->trapitems, trapitem);
		}

		/* log items */

		if (ITEM_VALUE_TYPE_LOG == item->value_type &&
				NULL != (logitem = zbx_hashset_search(&config->logitems, &itemid)))
		{
			zbx_strpool_release(logitem->logtimefmt);
			zbx_hashset_remove_direct(&config->logitems, logitem);
		}

		/* db items */

		if (ITEM_TYPE_DB_MONITOR == item->type &&
				NULL != (dbitem = zbx_hashset_search(&config->dbitems, &itemid)))
		{
			zbx_strpool_release(dbitem->params);
			zbx_strpool_release(dbitem->username);
			zbx_strpool_release(dbitem->password);

			zbx_hashset_remove_direct(&config->dbitems, dbitem);
		}

		/* SSH items */

		if (ITEM_TYPE_SSH == item->type)
		{
			sshitem = zbx_hashset_search(&config->sshitems, &itemid);

			zbx_strpool_release(sshitem->username);
			zbx_strpool_release(sshitem->password);
			zbx_strpool_release(sshitem->publickey);
			zbx_strpool_release(sshitem->privatekey);
			zbx_strpool_release(sshitem->params);

			zbx_hashset_remove_direct(&config->sshitems, sshitem);
		}

		/* TELNET items */

		if (ITEM_TYPE_TELNET == item->type)
		{
			telnetitem = zbx_hashset_search(&config->telnetitems, &itemid);

			zbx_strpool_release(telnetitem->username);
			zbx_strpool_release(telnetitem->password);
			zbx_strpool_release(telnetitem->params);

			zbx_hashset_remove_direct(&config->telnetitems, telnetitem);
		}

		/* simple items */

		if (ITEM_TYPE_SIMPLE == item->type)
		{
			simpleitem = zbx_hashset_search(&config->simpleitems, &itemid);

			zbx_strpool_release(simpleitem->username);
			zbx_strpool_release(simpleitem->password);

			zbx_hashset_remove_direct(&config->simpleitems, simpleitem);
		}

		/* JMX items */

		if (ITEM_TYPE_JMX == item->type)
		{
			jmxitem = zbx_hashset_search(&config->jmxitems, &itemid);

			zbx_strpool_release(jmxitem->username);
			zbx_strpool_release(jmxitem->password);

			zbx_hashset_remove_direct(&config->jmxitems, jmxitem);
		}

		/* calculated items */

		if (ITEM_TYPE_CALCULATED == item->type)
		{
			calcitem = zbx_hashset_search(&config->calcitems, &itemid);
			zbx_strpool_release(calcitem->params);
			zbx_hashset_remove_direct(&config->calcitems, calcitem);
		}

		/* delta items */

		if (NULL != (deltaitem = zbx_hashset_search(&config->deltaitems, &itemid)))
			zbx_hashset_remove_direct(&config->deltaitems, deltaitem);

		/* items */

		item_hk_local.hostid = item->hostid;
		item_hk_local.key = item->key;
		item_hk = zbx_hashset_search(&config->items_hk, &item_hk_local);

		if (item == item_hk->item_ptr)
		{
			zbx_strpool_release(item_hk->key);
			zbx_hashset_remove_direct(&config->items_hk, item_hk);
		}

		if (ZBX_LOC_QUEUE == item->location)
			zbx_binary_heap_remove_direct(&config->queues[item->poller_type], item->itemid);

		zbx_strpool_release(item->key);
		zbx_strpool_release(item->port);
		zbx_strpool_release(item->db_error);

		if (NULL != item->triggers)
			config->items.mem_free_func(item->triggers);

		zbx_hashset_remove_direct(&config->items, item);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void	DCsync_triggers(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_triggers";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_TRIGGER		*trigger;

	int			found, ret;
	zbx_uint64_t		triggerid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(triggerid, row[0]);

		trigger = DCfind_id(&config->triggers, triggerid, sizeof(ZBX_DC_TRIGGER), &found);

		/* store new information in trigger structure */

		DCstrpool_replace(found, &trigger->description, row[1]);
		DCstrpool_replace(found, &trigger->expression, row[2]);
		ZBX_STR2UCHAR(trigger->priority, row[4]);
		ZBX_STR2UCHAR(trigger->type, row[5]);
		ZBX_STR2UCHAR(trigger->status, row[9]);

		if (0 == found)
		{
			DCstrpool_replace(found, &trigger->error, row[3]);
			ZBX_STR2UCHAR(trigger->value, row[6]);
			ZBX_STR2UCHAR(trigger->state, row[7]);
			trigger->lastchange = atoi(row[8]);
			trigger->locked = 0;
			trigger->expression_ex = NULL;
			trigger->topoindex = 1;
		}
	}

	/* remove deleted triggers from buffer */
	if (SUCCEED == ret)
	{
		zbx_vector_uint64_t	functionids;
		int			i;
		ZBX_DC_ITEM		*item;
		ZBX_DC_FUNCTION		*function;

		zbx_vector_uint64_create(&functionids);

		for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
		{
			if (NULL == (trigger = zbx_hashset_search(&config->triggers, &rowid)))
				continue;

			/* force trigger list update for items used in removed trigger */
			get_functionids(&functionids, trigger->expression);
			for (i = 0; i < functionids.values_num; i++)
			{
				if (NULL == (function = zbx_hashset_search(&config->functions, &functionids.values[i])))
					continue;

				if (NULL == (item = zbx_hashset_search(&config->items, &function->itemid)))
					continue;

				item->update_triggers = 1;
				if (NULL != item->triggers)
				{
					config->items.mem_free_func(item->triggers);
					item->triggers = NULL;
				}
			}
			zbx_vector_uint64_clear(&functionids);

			zbx_strpool_release(trigger->description);
			zbx_strpool_release(trigger->expression);
			zbx_strpool_release(trigger->error);

			if (NULL != trigger->expression_ex)
				zbx_strpool_release(trigger->expression_ex);

			zbx_hashset_remove_direct(&config->triggers, trigger);
		}

		zbx_vector_uint64_destroy(&functionids);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void	DCconfig_sort_triggers_topologically(void);

/******************************************************************************
 *                                                                            *
 * Function: dc_trigger_deplist_release                                       *
 *                                                                            *
 * Purpose: releases trigger dependency list, removing it if necessary        *
 *                                                                            *
 ******************************************************************************/
static int	dc_trigger_deplist_release(ZBX_DC_TRIGGER_DEPLIST *trigdep)
{
	if (0 == --trigdep->refcount)
	{
		zbx_vector_ptr_destroy(&trigdep->dependencies);
		zbx_hashset_remove_direct(&config->trigdeps, trigdep);
		return SUCCEED;
	}

	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_trigger_deplist_init                                          *
 *                                                                            *
 * Purpose: initializes trigger dependency list                               *
 *                                                                            *
 ******************************************************************************/
static void	dc_trigger_deplist_init(ZBX_DC_TRIGGER_DEPLIST *trigdep, ZBX_DC_TRIGGER *trigger)
{
	trigdep->refcount = 1;
	trigdep->trigger = trigger;
	zbx_vector_ptr_create_ext(&trigdep->dependencies, __config_mem_malloc_func, __config_mem_realloc_func,
			__config_mem_free_func);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_trigger_deplist_reset                                         *
 *                                                                            *
 * Purpose: resets trigger dependency list to release memory allocated by     *
 *          dependencies vector                                               *
 *                                                                            *
 ******************************************************************************/
static void	dc_trigger_deplist_reset(ZBX_DC_TRIGGER_DEPLIST *trigdep)
{
	zbx_vector_ptr_destroy(&trigdep->dependencies);
	zbx_vector_ptr_create_ext(&trigdep->dependencies, __config_mem_malloc_func, __config_mem_realloc_func,
			__config_mem_free_func);
}

static void	DCsync_trigdeps(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_trigdeps";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_TRIGGER_DEPLIST	*trigdep_down, *trigdep_up;

	int			found, index, ret;
	zbx_uint64_t		triggerid_down, triggerid_up;
	ZBX_DC_TRIGGER		*trigger_up, *trigger_down;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		/* find trigdep_down pointer */

		ZBX_STR2UINT64(triggerid_down, row[0]);
		if (NULL == (trigger_down = zbx_hashset_search(&config->triggers, &triggerid_down)))
			continue;

		ZBX_STR2UINT64(triggerid_up, row[1]);
		if (NULL == (trigger_up = zbx_hashset_search(&config->triggers, &triggerid_up)))
			continue;

		trigdep_down = DCfind_id(&config->trigdeps, triggerid_down, sizeof(ZBX_DC_TRIGGER_DEPLIST), &found);
		if (0 == found)
			dc_trigger_deplist_init(trigdep_down, trigger_down);
		else
			trigdep_down->refcount++;

		trigdep_up = DCfind_id(&config->trigdeps, triggerid_up, sizeof(ZBX_DC_TRIGGER_DEPLIST), &found);
		if (0 == found)
			dc_trigger_deplist_init(trigdep_up, trigger_up);
		else
			trigdep_up->refcount++;

		zbx_vector_ptr_append(&trigdep_down->dependencies, trigdep_up);
	}

	/* remove deleted trigger dependencies from buffer */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		ZBX_STR2UINT64(triggerid_down, row[0]);
		if (NULL == (trigdep_down = (ZBX_DC_TRIGGER_DEPLIST *)zbx_hashset_search(&config->trigdeps,
				&triggerid_down)))
		{
			continue;
		}

		ZBX_STR2UINT64(triggerid_up, row[1]);
		if (NULL != (trigdep_up = (ZBX_DC_TRIGGER_DEPLIST *)zbx_hashset_search(&config->trigdeps,
				&triggerid_up)))
		{
			dc_trigger_deplist_release(trigdep_up);
		}

		if (SUCCEED != dc_trigger_deplist_release(trigdep_down))
		{
			if (FAIL == (index = zbx_vector_ptr_search(&trigdep_down->dependencies, &triggerid_up,
					ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC)))
			{
				continue;
			}

			if (1 == trigdep_down->dependencies.values_num)
				dc_trigger_deplist_reset(trigdep_down);
			else
				zbx_vector_ptr_remove_noorder(&trigdep_down->dependencies, index);
		}
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void	DCsync_functions(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_functions";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;

	ZBX_DC_ITEM		*item;
	ZBX_DC_FUNCTION		*function;
	ZBX_DC_TRIGGER		*trigger;

	int			found, ret;
	zbx_uint64_t		itemid, functionid, triggerid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(itemid, row[0]);
		ZBX_STR2UINT64(functionid, row[1]);
		ZBX_STR2UINT64(triggerid, row[4]);

		if (NULL == (item = zbx_hashset_search(&config->items, &itemid)) ||
				NULL == (trigger = zbx_hashset_search(&config->triggers, &triggerid)))
		{
			/* Item and trigger could have been created after we have selected them in the */
			/* previous queries. However, we shall avoid the check for functions being the */
			/* same as in the trigger expression, because that is somewhat expensive, not */
			/* 100% (think functions keeping their functionid, but changing their function */
			/* or parameters), and even if there is an inconsistency, we can live with it. */

			continue;
		}

		/* process function information */

		function = DCfind_id(&config->functions, functionid, sizeof(ZBX_DC_FUNCTION), &found);

		function->triggerid = triggerid;
		function->itemid = itemid;
		DCstrpool_replace(found, &function->function, row[2]);
		DCstrpool_replace(found, &function->parameter, row[3]);

		function->timer = (SUCCEED == is_time_function(function->function) ? 1 : 0);

		item->update_triggers = 1;
		if (NULL != item->triggers)
		{
			if (ZBX_DBSYNC_ROW_REMOVE == tag)
			{
				config->items.mem_free_func(item->triggers);
				item->triggers = NULL;
			}
			else
				item->triggers[0] = NULL;
		}
	}

	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (function = zbx_hashset_search(&config->functions, &rowid)))
			continue;

		if (NULL != (item = zbx_hashset_search(&config->items, &function->itemid)))
		{
			item->update_triggers = 1;
			if (NULL != item->triggers)
			{
				config->items.mem_free_func(item->triggers);
				item->triggers = NULL;
			}
		}

		zbx_strpool_release(function->function);
		zbx_strpool_release(function->parameter);

		zbx_hashset_remove_direct(&config->functions, function);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}


/******************************************************************************
 *                                                                            *
 * Function: dc_regexp_remove_expression                                      *
 *                                                                            *
 * Purpose: removes expression from regexp                                    *
 *                                                                            *
 ******************************************************************************/
static ZBX_DC_REGEXP	*dc_regexp_remove_expression(const char *regexp_name, zbx_uint64_t expressionid)
{
	ZBX_DC_REGEXP	*regexp, regexp_local;
	int		index;

	regexp_local.name = regexp_name;

	if (NULL == (regexp = zbx_hashset_search(&config->regexps, &regexp_local)))
		return NULL;

	if (FAIL == (index = zbx_vector_uint64_search(&regexp->expressionids, expressionid,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC)))
	{
		return NULL;
	}

	zbx_vector_uint64_remove_noorder(&regexp->expressionids, index);

	return regexp;
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_expressions                                               *
 *                                                                            *
 * Purpose: Updates expressions configuration cache                           *
 *                                                                            *
 * Parameters: result - [IN] the result of expressions database select        *
 *                                                                            *
 ******************************************************************************/
static void	DCsync_expressions(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_expressions";
	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;
	zbx_hashset_iter_t	iter;
	ZBX_DC_EXPRESSION	*expression;
	ZBX_DC_REGEXP		*regexp, regexp_local;
	zbx_uint64_t		expressionid;
	int 			found, ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(expressionid, row[1]);
		expression = DCfind_id(&config->expressions, expressionid, sizeof(ZBX_DC_EXPRESSION), &found);

		if (0 != found)
			dc_regexp_remove_expression(expression->regexp, expressionid);

		DCstrpool_replace(found, &expression->regexp, row[0]);
		DCstrpool_replace(found, &expression->expression, row[2]);
		ZBX_STR2UCHAR(expression->type, row[3]);
		ZBX_STR2UCHAR(expression->case_sensitive, row[5]);
		expression->delimiter = *row[4];

		regexp_local.name = row[0];

		if (NULL == (regexp = zbx_hashset_search(&config->regexps, &regexp_local)))
		{
			DCstrpool_replace(0, &regexp_local.name, row[0]);
			zbx_vector_uint64_create_ext(&regexp_local.expressionids,
					__config_mem_malloc_func,
					__config_mem_realloc_func,
					__config_mem_free_func);

			regexp = zbx_hashset_insert(&config->regexps, &regexp_local, sizeof(ZBX_DC_REGEXP));
		}

		zbx_vector_uint64_append(&regexp->expressionids, expressionid);
	}

	/* remove regexps with no expressions related to it */
	zbx_hashset_iter_reset(&config->regexps, &iter);

	while (NULL != (regexp = zbx_hashset_iter_next(&iter)))
	{
		if (0 < regexp->expressionids.values_num)
			continue;

		zbx_strpool_release(regexp->name);
		zbx_vector_uint64_destroy(&regexp->expressionids);
		zbx_hashset_iter_remove(&iter);
	}

	/* remove unused expressions */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (expression = zbx_hashset_search(&config->expressions, &rowid)))
			continue;

		if (NULL != (regexp = dc_regexp_remove_expression(expression->regexp, expression->expressionid)))
		{
			if (0 == regexp->expressionids.values_num)
			{
				zbx_strpool_release(regexp->name);
				zbx_vector_uint64_destroy(&regexp->expressionids);
				zbx_hashset_remove_direct(&config->regexps, regexp);
			}
		}

		zbx_strpool_release(expression->expression);
		zbx_strpool_release(expression->regexp);
		zbx_hashset_remove_direct(&config->expressions, expression);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_actions                                                   *
 *                                                                            *
 * Purpose: Updates actions configuration cache                               *
 *                                                                            *
 * Parameters: result - [IN] the result of actions database select            *
 *                                                                            *
 * Comments: The result contains the following fields:                        *
 *           0 - actionid                                                     *
 *           1 - eventsource                                                  *
 *           2 - evaltype                                                     *
 *           3 - formula                                                      *
 *                                                                            *
 ******************************************************************************/
static void	DCsync_actions(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_actions";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;
	zbx_uint64_t		actionid;
	zbx_dc_action_t		*action;
	int			found, ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(actionid, row[0]);
		action = DCfind_id(&config->actions, actionid, sizeof(zbx_dc_action_t), &found);

		if (0 == found)
		{
			zbx_vector_ptr_create_ext(&action->conditions, __config_mem_malloc_func,
					__config_mem_realloc_func, __config_mem_free_func);

			zbx_vector_ptr_reserve(&action->conditions, 1);
		}

		ZBX_STR2UCHAR(action->eventsource, row[1]);
		ZBX_STR2UCHAR(action->evaltype, row[2]);

		DCstrpool_replace(found, &action->formula, row[3]);
	}

	/* remove deleted actions */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (action = zbx_hashset_search(&config->actions, &rowid)))
			continue;

		zbx_strpool_release(action->formula);
		zbx_vector_ptr_destroy(&action->conditions);

		zbx_hashset_remove_direct(&config->actions, action);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_compare_action_conditions_by_type                             *
 *                                                                            *
 * Purpose: compare two action conditions by their type                       *
 *                                                                            *
 * Comments: This function is used to sort action conditions by type.         *
 *                                                                            *
 ******************************************************************************/
static int	dc_compare_action_conditions_by_type(const void *d1, const void *d2)
{
	zbx_dc_action_condition_t	*c1 = *(zbx_dc_action_condition_t **)d1;
	zbx_dc_action_condition_t	*c2 = *(zbx_dc_action_condition_t **)d2;

	ZBX_RETURN_IF_NOT_EQUAL(c1->conditiontype, c2->conditiontype);

	return 0;
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_action_conditions                                         *
 *                                                                            *
 * Purpose: Updates action conditions configuration cache                     *
 *                                                                            *
 * Parameters: result - [IN] the result of action conditions database select  *
 *                                                                            *
 * Comments: The result contains the following fields:                        *
 *           0 - conditionid                                                  *
 *           1 - actionid                                                     *
 *           2 - conditiontype                                                *
 *           3 - operator                                                     *
 *           4 - value                                                        *
 *                                                                            *
 ******************************************************************************/
static void	DCsync_action_conditions(zbx_dbsync_t *sync)
{
	const char			*__function_name = "DCsync_action_conditions";

	char				**row;
	zbx_uint64_t			rowid;
	unsigned char			tag;
	zbx_uint64_t			actionid, conditionid;
	zbx_dc_action_t			*action;
	zbx_dc_action_condition_t	*condition;
	int				found, i, index, ret;
	zbx_vector_ptr_t		actions;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_ptr_create(&actions);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(actionid, row[1]);

		if (NULL == (action = zbx_hashset_search(&config->actions, &actionid)))
			continue;

		ZBX_STR2UINT64(conditionid, row[0]);

		condition = DCfind_id(&config->action_conditions, conditionid, sizeof(zbx_dc_action_condition_t),
				&found);

		condition->actionid = actionid;
		ZBX_STR2UCHAR(condition->conditiontype, row[2]);
		ZBX_STR2UCHAR(condition->operator, row[3]);

		DCstrpool_replace(found, &condition->value, row[4]);

		if (0 == found)
			zbx_vector_ptr_append(&action->conditions, condition);

		zbx_vector_ptr_append(&actions, action);
	}

	/* remove deleted conditions */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (condition = zbx_hashset_search(&config->action_conditions, &rowid)))
			continue;

		if (NULL != (action = zbx_hashset_search(&config->actions, &condition->actionid)))
		{
			if (FAIL != (index = zbx_vector_ptr_search(&action->conditions, condition,
					ZBX_DEFAULT_PTR_COMPARE_FUNC)))
			{
				zbx_vector_ptr_remove_noorder(&action->conditions, index);
				zbx_vector_ptr_append(&actions, action);
			}
		}

		zbx_strpool_release(condition->value);

		zbx_hashset_remove_direct(&config->action_conditions, condition);
	}

	/* sort conditions by type */

	zbx_vector_ptr_sort(&actions, ZBX_DEFAULT_PTR_COMPARE_FUNC);
	zbx_vector_ptr_uniq(&actions, ZBX_DEFAULT_PTR_COMPARE_FUNC);

	for (i = 0; i < actions.values_num; i++)
	{
		action = (zbx_dc_action_t *)actions.values[i];

		if (CONDITION_EVAL_TYPE_AND_OR == action->evaltype)
			zbx_vector_ptr_sort(&action->conditions, dc_compare_action_conditions_by_type);
	}

	zbx_vector_ptr_destroy(&actions);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_trigger_reset_cached_expressions                              *
 *                                                                            *
 * Purpose: resets cached trigger expressions to ensure the expression is     *
 *          updated after expression/macro changes                            *
 *                                                                            *
 ******************************************************************************/
static void	dc_trigger_reset_cached_expressions()
{
	zbx_hashset_iter_t	iter;
	ZBX_DC_TRIGGER		*trigger;

	zbx_hashset_iter_reset(&config->triggers, &iter);
	while (NULL != (trigger = (ZBX_DC_TRIGGER *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL != trigger->expression_ex)
		{
			zbx_strpool_release(trigger->expression_ex);
			trigger->expression_ex = NULL;
		}
	}
}

/******************************************************************************
 *                                                                            *
 * Function: dc_trigger_update_topology                                       *
 *                                                                            *
 * Purpose: updates trigger topology after trigger dependency changes         *
 *                                                                            *
 ******************************************************************************/
static void	dc_trigger_update_topology()
{
	zbx_hashset_iter_t	iter;
	ZBX_DC_TRIGGER		*trigger;

	zbx_hashset_iter_reset(&config->triggers, &iter);
	while (NULL != (trigger = (ZBX_DC_TRIGGER *)zbx_hashset_iter_next(&iter)))
		trigger->topoindex = 1;

	DCconfig_sort_triggers_topologically();
}

static int	zbx_default_ptr_pair_ptr_compare_func(const void *d1, const void *d2)
{
	const zbx_ptr_pair_t	*p1 = (const zbx_ptr_pair_t *)d1;
	const zbx_ptr_pair_t	*p2 = (const zbx_ptr_pair_t *)d2;

	ZBX_RETURN_IF_NOT_EQUAL(p1->first, p2->first);
	ZBX_RETURN_IF_NOT_EQUAL(p1->second, p2->second);

	return 0;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_trigger_update_cache                                          *
 *                                                                            *
 * Purpose: updates trigger related cache data;                               *
 *              1) time triggers assigned to timer processes                  *
 *              2) trigger functionality (if it uses contain disabled         *
 *                 items/hosts)                                               *
 *              3) list of triggers each item is used by                      *
 *                                                                            *
 ******************************************************************************/
static void	dc_trigger_update_cache()
{
	zbx_hashset_iter_t	iter;
	ZBX_DC_TRIGGER		*trigger;
	ZBX_DC_FUNCTION		*function;
	ZBX_DC_ITEM		*item;
	int			i, j, k;
	zbx_ptr_pair_t		itemtrig;
	zbx_vector_ptr_pair_t	itemtrigs;

	zbx_hashset_iter_reset(&config->triggers, &iter);
	while (NULL != (trigger = (ZBX_DC_TRIGGER *)zbx_hashset_iter_next(&iter)))
		trigger->functional = TRIGGER_FUNCTIONAL_TRUE;

	for (i = 0; i < CONFIG_TIMER_FORKS; i++)
		zbx_vector_ptr_clear(&config->time_triggers[i]);

	zbx_vector_ptr_pair_create(&itemtrigs);
	zbx_hashset_iter_reset(&config->functions, &iter);
	while (NULL != (function = (ZBX_DC_FUNCTION *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == (item = zbx_hashset_search(&config->items, &function->itemid)) ||
				NULL == (trigger = zbx_hashset_search(&config->triggers, &function->triggerid)))
		{
			continue;
		}

		/* cache item - trigger link */
		if (0 != item->update_triggers)
		{
			itemtrig.first = item;
			itemtrig.second = trigger;
			zbx_vector_ptr_pair_append(&itemtrigs, itemtrig);
		}

		/* spread triggers with time-based functions between timer processes (load balancing) */
		if (1 == function->timer)
		{
			i = function->triggerid % CONFIG_TIMER_FORKS;
			zbx_vector_ptr_append(&config->time_triggers[i], trigger);
		}

		/* disable functionality for triggers with expression containing */
		/* disabled or not monitored items                               */
		if (TRIGGER_FUNCTIONAL_FALSE != trigger->functional)
		{
			if (ITEM_STATUS_DISABLED != item->status)
			{
				ZBX_DC_HOST	*host;

				host = zbx_hashset_search(&config->hosts, &item->hostid);

				if (HOST_STATUS_NOT_MONITORED != host->status)
					continue;
			}

			trigger->functional = TRIGGER_FUNCTIONAL_FALSE;
		}
	}

	for (i = 0; i < CONFIG_TIMER_FORKS; i++)
	{
		zbx_vector_ptr_sort(&config->time_triggers[i], ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);
		zbx_vector_ptr_uniq(&config->time_triggers[i], ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);
	}

	zbx_vector_ptr_pair_sort(&itemtrigs, zbx_default_ptr_pair_ptr_compare_func);
	zbx_vector_ptr_pair_uniq(&itemtrigs, zbx_default_ptr_pair_ptr_compare_func);

	/* update links from items to triggers */
	for (i = 0; i < itemtrigs.values_num; i++)
	{
		for (j = i + 1; j < itemtrigs.values_num; j++)
		{
			if (itemtrigs.values[i].first != itemtrigs.values[j].first)
				break;
		}

		item = (ZBX_DC_ITEM *)itemtrigs.values[i].first;
		item->update_triggers = 0;
		item->triggers = config->items.mem_realloc_func(item->triggers, (j - i + 1) * sizeof(ZBX_DC_TRIGGER *));

		for (k = i; k < j; k++)
			item->triggers[k - i] = (ZBX_DC_TRIGGER *)itemtrigs.values[k].second;

		item->triggers[j - i] = NULL;

		i = j - 1;
	}

	zbx_vector_ptr_pair_destroy(&itemtrigs);
}

static void	DCdump_config()
{
	const char	*__function_name = "DCdump_config";

	int		i;

	if (NULL == config->config)
	{
		zabbix_log(LOG_LEVEL_TRACE, "cannot parse config table");
		return;
	}

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zabbix_log(LOG_LEVEL_TRACE, "  refresh_unsupported %d", config->config->refresh_unsupported);
	zabbix_log(LOG_LEVEL_TRACE, "  discovery_groupid " ZBX_FS_UI64, config->config->discovery_groupid);
	zabbix_log(LOG_LEVEL_TRACE, "  snmptrap_logging %u", config->config->snmptrap_logging);
	zabbix_log(LOG_LEVEL_TRACE, "  default_inventory_mode %d", config->config->default_inventory_mode);

	for (i = 0; TRIGGER_SEVERITY_COUNT > i; i++)
		zabbix_log(LOG_LEVEL_TRACE, "  severity name '%s'", config->config->severity_name[i]);

	zabbix_log(LOG_LEVEL_TRACE, "  hk.events_mode %u", config->config->hk.events_mode);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.events_trigger %d", config->config->hk.events_trigger);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.events_internal %d", config->config->hk.events_internal);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.events_autoreg %d", config->config->hk.events_autoreg);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.events_discovery %d", config->config->hk.events_discovery);

	zabbix_log(LOG_LEVEL_TRACE, "  hk.audit_mode %u", config->config->hk.audit_mode);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.audit %d", config->config->hk.audit);

	zabbix_log(LOG_LEVEL_TRACE, "  hk.services_mode %u", config->config->hk.services_mode);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.services %d", config->config->hk.services);

	zabbix_log(LOG_LEVEL_TRACE, "  hk.sessions_mode %u", config->config->hk.sessions_mode);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.sessions %d", config->config->hk.sessions);

	zabbix_log(LOG_LEVEL_TRACE, "  hk.history_mode %u", config->config->hk.history_mode);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.history_global %u", config->config->hk.history_global);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.history %d", config->config->hk.history);

	zabbix_log(LOG_LEVEL_TRACE, "  hk.trends_mode %u", config->config->hk.trends_mode);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.trends_global %u", config->config->hk.trends_global);
	zabbix_log(LOG_LEVEL_TRACE, "  hk.trends %d", config->config->hk.trends);

	zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_hosts()
{
	const char		*__function_name = "DCdump_hosts";

	const ZBX_DC_HOST	*host;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->hosts, &iter);

	while (NULL != (host = (ZBX_DC_HOST *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  hostid " ZBX_FS_UI64, host->hostid);
		zabbix_log(LOG_LEVEL_TRACE, "  proxy_hostid " ZBX_FS_UI64, host->proxy_hostid);
		zabbix_log(LOG_LEVEL_TRACE, "  host '%s'", host->host);
		zabbix_log(LOG_LEVEL_TRACE, "  name '%s'", host->name);
		zabbix_log(LOG_LEVEL_TRACE, "  maintenance_from %d", host->maintenance_from);
		zabbix_log(LOG_LEVEL_TRACE, "  data_expected_from %d", host->data_expected_from);
		zabbix_log(LOG_LEVEL_TRACE, "  errors_from %d", host->errors_from);
		zabbix_log(LOG_LEVEL_TRACE, "  disable_until %d", host->disable_until);
		zabbix_log(LOG_LEVEL_TRACE, "  snmp_errors_from %d", host->snmp_errors_from);
		zabbix_log(LOG_LEVEL_TRACE, "  snmp_disable_until %d", host->snmp_disable_until);
		zabbix_log(LOG_LEVEL_TRACE, "  ipmi_errors_from %d", host->ipmi_errors_from);
		zabbix_log(LOG_LEVEL_TRACE, "  ipmi_disable_until %d", host->ipmi_disable_until);
		zabbix_log(LOG_LEVEL_TRACE, "  jmx_errors_from %d", host->jmx_errors_from);
		zabbix_log(LOG_LEVEL_TRACE, "  jmx_disable_until %d", host->jmx_disable_until);

		/* timestamp of last availability status (available/error) field change on any interface */
		zabbix_log(LOG_LEVEL_TRACE, "  availability_ts %d", host->availability_ts);

		zabbix_log(LOG_LEVEL_TRACE, "  maintenance_status %u", host->maintenance_status);
		zabbix_log(LOG_LEVEL_TRACE, "  maintenance_type %u", host->maintenance_type);
		zabbix_log(LOG_LEVEL_TRACE, "  available %u", host->available);
		zabbix_log(LOG_LEVEL_TRACE, "  snmp_available %u", host->snmp_available);
		zabbix_log(LOG_LEVEL_TRACE, "  ipmi_available %u", host->ipmi_available);
		zabbix_log(LOG_LEVEL_TRACE, "  jmx_available %u", host->jmx_available);
		zabbix_log(LOG_LEVEL_TRACE, "  status %u", host->status);

		/* specifies which interfaces are being used (have enabled items) */
		/* (see ZBX_FLAG_INTERFACE_* defines)                             */
		zabbix_log(LOG_LEVEL_TRACE, "  used_interfaces %u", host->used_interfaces);

		/* 'tls_connect' and 'tls_accept' must be respected even if encryption support is not compiled in */
		zabbix_log(LOG_LEVEL_TRACE, "  tls_connect %u", host->tls_connect);
		zabbix_log(LOG_LEVEL_TRACE, "  tls_accept %u", host->tls_accept);
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
		zabbix_log(LOG_LEVEL_TRACE, "  tls_issuer '%s'", host->tls_issuer);
		zabbix_log(LOG_LEVEL_TRACE, "  tls_subject '%s'", host->tls_subject);

		if (NULL != host->tls_dc_psk)
		{
			zabbix_log(LOG_LEVEL_TRACE, "  tls_psk_identity '%s'", host->tls_dc_psk->tls_psk_identity);
			zabbix_log(LOG_LEVEL_TRACE, "  tls_psk '%s'", host->tls_dc_psk->tls_psk);
			zabbix_log(LOG_LEVEL_TRACE, "  tls_dc_psk %u", host->tls_dc_psk->refcount);
		}
#endif
		zabbix_log(LOG_LEVEL_TRACE, "  error '%s'", host->error);
		zabbix_log(LOG_LEVEL_TRACE, "  snmp_error '%s'", host->snmp_error);
		zabbix_log(LOG_LEVEL_TRACE, "  ipmi_error '%s'", host->ipmi_error);
		zabbix_log(LOG_LEVEL_TRACE, "  jmx_error '%s'", host->jmx_error);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_proxies()
{
	const char		*__function_name = "DCdump_proxies";

	const ZBX_DC_PROXY	*proxy;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->proxies, &iter);

	while (NULL != (proxy = (ZBX_DC_PROXY *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  hostid " ZBX_FS_UI64, proxy->hostid);
		zabbix_log(LOG_LEVEL_TRACE, "  proxy_config_nextcheck %d", proxy->proxy_config_nextcheck);
		zabbix_log(LOG_LEVEL_TRACE, "  proxy_data_nextcheck %d", proxy->proxy_data_nextcheck);
		zabbix_log(LOG_LEVEL_TRACE, "  timediff %d", proxy->timediff);
		zabbix_log(LOG_LEVEL_TRACE, "  lastaccess %d", proxy->lastaccess);
		zabbix_log(LOG_LEVEL_TRACE, "  location %u", proxy->location);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_ipmihosts()
{
	const char		*__function_name = "DCdump_ipmihosts";

	const ZBX_DC_IPMIHOST	*ipmihost;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->ipmihosts, &iter);

	while (NULL != (ipmihost = (ZBX_DC_IPMIHOST *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  hostid " ZBX_FS_UI64, ipmihost->hostid);
		zabbix_log(LOG_LEVEL_TRACE, "  ipmi_username '%s' ipmi_password '%s'", ipmihost->ipmi_username,
				ipmihost->ipmi_password);
		zabbix_log(LOG_LEVEL_TRACE, "  ipmi_authtype %d", ipmihost->ipmi_authtype);
		zabbix_log(LOG_LEVEL_TRACE, "  ipmi_privilege %u", ipmihost->ipmi_privilege);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_host_inventories()
{
	const char			*__function_name = "DCdump_host_inventories";

	const ZBX_DC_HOST_INVENTORY	*host_inventory;
	zbx_hashset_iter_t		iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->host_inventories, &iter);

	while (NULL != (host_inventory = (ZBX_DC_HOST_INVENTORY *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  hostid " ZBX_FS_UI64, host_inventory->hostid);
		zabbix_log(LOG_LEVEL_TRACE, "  inventory_mode %u", host_inventory->inventory_mode);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_htmpls()
{
	const char		*__function_name = "DCdump_htmpls";

	const ZBX_DC_HTMPL	*htmpl = NULL;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->htmpls, &iter);

	while (NULL != (htmpl = (ZBX_DC_HTMPL *)zbx_hashset_iter_next(&iter)))
	{
		int	i;

		zabbix_log(LOG_LEVEL_TRACE, "  hostid " ZBX_FS_UI64, htmpl->hostid);

		for (i = 0; i < htmpl->templateids.values_num; i++)
			zabbix_log(LOG_LEVEL_TRACE, "  templateid " ZBX_FS_UI64, htmpl->templateids.values[i]);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_gmacros()
{
	const char		*__function_name = "DCdump_gmacro";

	const ZBX_DC_GMACRO	*gmacro;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->gmacros, &iter);

	while (NULL != (gmacro = (ZBX_DC_GMACRO *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  globalmacroid " ZBX_FS_UI64, gmacro->globalmacroid);
		zabbix_log(LOG_LEVEL_TRACE, "  macro '%s'", gmacro->macro);
		if (NULL != gmacro->context)
			zabbix_log(LOG_LEVEL_TRACE, "  context '%s'", gmacro->context);
		zabbix_log(LOG_LEVEL_TRACE, "  value '%s'", gmacro->value);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_hmacros()
{
	const char		*__function_name = "DCdump_hmacros";

	const ZBX_DC_HMACRO	*hmacro;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->hmacros, &iter);

	while (NULL != (hmacro = (ZBX_DC_HMACRO *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  hostmacroid " ZBX_FS_UI64, hmacro->hostmacroid);
		zabbix_log(LOG_LEVEL_TRACE, "  hostid " ZBX_FS_UI64, hmacro->hostid);

		zabbix_log(LOG_LEVEL_TRACE, "  macro '%s'", hmacro->macro);
		if (NULL != hmacro->context)
			zabbix_log(LOG_LEVEL_TRACE, "  context '%s'", hmacro->context);
		zabbix_log(LOG_LEVEL_TRACE, "  value '%s'", hmacro->value);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_interfaces()
{
	const char		*__function_name = "DCdump_interfaces";

	const ZBX_DC_INTERFACE	*interface;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->interfaces, &iter);

	while (NULL != (interface = (ZBX_DC_INTERFACE *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  interfaceid " ZBX_FS_UI64, interface->interfaceid);
		zabbix_log(LOG_LEVEL_TRACE, "  hostid " ZBX_FS_UI64, interface->hostid);

		zabbix_log(LOG_LEVEL_TRACE, "  ip '%s'", interface->ip);
		zabbix_log(LOG_LEVEL_TRACE, "  dns '%s'", interface->dns);
		zabbix_log(LOG_LEVEL_TRACE, "  port '%s'", interface->port);

		zabbix_log(LOG_LEVEL_TRACE, "  type %u", interface->type);
		zabbix_log(LOG_LEVEL_TRACE, "  main %u", interface->main);
		zabbix_log(LOG_LEVEL_TRACE, "  useip %u", interface->useip);
		zabbix_log(LOG_LEVEL_TRACE, "  bulk %u", interface->bulk);
		zabbix_log(LOG_LEVEL_TRACE, "  max_snmp_succeed %u", interface->max_snmp_succeed);
		zabbix_log(LOG_LEVEL_TRACE, "  min_snmp_fail %u", interface->min_snmp_fail);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_trigger(const ZBX_DC_TRIGGER	*trigger)
{
	zabbix_log(LOG_LEVEL_TRACE, "    ====================");

	zabbix_log(LOG_LEVEL_TRACE, "    triggerid " ZBX_FS_UI64, trigger->triggerid);

	zabbix_log(LOG_LEVEL_TRACE, "    description '%s'", trigger->description);
	zabbix_log(LOG_LEVEL_TRACE, "    expression '%s'", trigger->expression);

	if (NULL != trigger->expression_ex)
		zabbix_log(LOG_LEVEL_TRACE, "    expression_ex '%s'", trigger->expression_ex);
	zabbix_log(LOG_LEVEL_TRACE, "    error '%s'", trigger->error);

	zabbix_log(LOG_LEVEL_TRACE, "    lastchange %d", trigger->lastchange);

	zabbix_log(LOG_LEVEL_TRACE, "    topoindex %u", trigger->topoindex);
	zabbix_log(LOG_LEVEL_TRACE, "    priority %u", trigger->priority);
	zabbix_log(LOG_LEVEL_TRACE, "    type %u", trigger->type);
	zabbix_log(LOG_LEVEL_TRACE, "    value %u", trigger->value);
	zabbix_log(LOG_LEVEL_TRACE, "    state %u", trigger->state);
	zabbix_log(LOG_LEVEL_TRACE, "    locked %u", trigger->locked);
	zabbix_log(LOG_LEVEL_TRACE, "    status %u", trigger->status);
	zabbix_log(LOG_LEVEL_TRACE, "    functional %u", trigger->functional);
}

static void	DCdump_items()
{
	const char		*__function_name = "DCdump_items";

	const ZBX_DC_ITEM	*item;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->items, &iter);

	while (NULL != (item = (ZBX_DC_ITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, item->itemid);
		zabbix_log(LOG_LEVEL_TRACE, "  hostid " ZBX_FS_UI64, item->hostid);
		zabbix_log(LOG_LEVEL_TRACE, "  interfaceid " ZBX_FS_UI64, item->interfaceid);
		zabbix_log(LOG_LEVEL_TRACE, "  lastlogsize " ZBX_FS_UI64, item->lastlogsize);
		zabbix_log(LOG_LEVEL_TRACE, "  valuemapid " ZBX_FS_UI64, item->valuemapid);
		zabbix_log(LOG_LEVEL_TRACE, "  key '%s'", item->key);
		zabbix_log(LOG_LEVEL_TRACE, "  port '%s'", item->port);
		zabbix_log(LOG_LEVEL_TRACE, "  db_error '%s'", item->db_error);

		zabbix_log(LOG_LEVEL_TRACE, "  delay %d", item->delay);
		zabbix_log(LOG_LEVEL_TRACE, "  nextcheck %d", item->nextcheck);
		zabbix_log(LOG_LEVEL_TRACE, "  lastclock %d", item->lastclock);
		zabbix_log(LOG_LEVEL_TRACE, "  mtime %d", item->mtime);
		zabbix_log(LOG_LEVEL_TRACE, "  data_expected_from %d", item->data_expected_from);
		zabbix_log(LOG_LEVEL_TRACE, "  history %d", item->history);
		zabbix_log(LOG_LEVEL_TRACE, "  type %u", item->type);
		zabbix_log(LOG_LEVEL_TRACE, "  data_type %u", item->data_type);
		zabbix_log(LOG_LEVEL_TRACE, "  value_type %u", item->value_type);
		zabbix_log(LOG_LEVEL_TRACE, "  poller_type %u", item->poller_type);
		zabbix_log(LOG_LEVEL_TRACE, "  state %u", item->state);
		zabbix_log(LOG_LEVEL_TRACE, "  db_state %u", item->db_state);
		zabbix_log(LOG_LEVEL_TRACE, "  inventory_link %u", item->inventory_link);
		zabbix_log(LOG_LEVEL_TRACE, "  location %u", item->location);
		zabbix_log(LOG_LEVEL_TRACE, "  flags %u", item->flags);
		zabbix_log(LOG_LEVEL_TRACE, "  status %u", item->status);
		zabbix_log(LOG_LEVEL_TRACE, "  unreachable %u", item->unreachable);
		zabbix_log(LOG_LEVEL_TRACE, "  update_triggers %u", item->update_triggers);

		if (NULL != item->triggers)
		{
			int		i;
			ZBX_DC_TRIGGER	*trigger;

			for (i = 0; NULL != (trigger = item->triggers[i]); i++)
				DCdump_trigger(trigger);
		}

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_numitems()
{
	const char		*__function_name = "DCdump_numitems";

	const ZBX_DC_NUMITEM	*numitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->numitems, &iter);

	while (NULL != (numitem = (ZBX_DC_NUMITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, numitem->itemid);

		zabbix_log(LOG_LEVEL_TRACE, "  formula '%s'", numitem->formula);
		zabbix_log(LOG_LEVEL_TRACE, "  units '%s'", numitem->units);
		zabbix_log(LOG_LEVEL_TRACE, "  trends %d", numitem->trends);
		zabbix_log(LOG_LEVEL_TRACE, "  delta %u", numitem->delta);
		zabbix_log(LOG_LEVEL_TRACE, "  multiplier %u", numitem->multiplier);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_snmpitems()
{
	const char		*__function_name = "DCdump_snmpitems";

	const ZBX_DC_SNMPITEM	*snmpitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->snmpitems, &iter);

	while (NULL != (snmpitem = (ZBX_DC_SNMPITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, snmpitem->itemid);
		zabbix_log(LOG_LEVEL_TRACE, "  snmp_oid '%s'", snmpitem->snmp_oid);
		zabbix_log(LOG_LEVEL_TRACE, "  snmp_community '%s'", snmpitem->snmp_community);
		zabbix_log(LOG_LEVEL_TRACE, "  snmpv3_securityname '%s'", snmpitem->snmpv3_securityname);
		zabbix_log(LOG_LEVEL_TRACE, "  snmpv3_authpassphrase '%s'", snmpitem->snmpv3_authpassphrase);
		zabbix_log(LOG_LEVEL_TRACE, "  snmpv3_privpassphrase '%s'", snmpitem->snmpv3_privpassphrase);
		zabbix_log(LOG_LEVEL_TRACE, "  snmpv3_contextname '%s'", snmpitem->snmpv3_contextname);
		zabbix_log(LOG_LEVEL_TRACE, "  snmpv3_securitylevel %u", snmpitem->snmpv3_securitylevel);
		zabbix_log(LOG_LEVEL_TRACE, "  snmpv3_authprotocol %u", snmpitem->snmpv3_authprotocol);
		zabbix_log(LOG_LEVEL_TRACE, "  snmpv3_privprotocol %u", snmpitem->snmpv3_privprotocol);
		zabbix_log(LOG_LEVEL_TRACE, "  snmp_oid_type %u", snmpitem->snmp_oid_type);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_ipmiitems()
{
	const char		*__function_name = "DCdump_ipmiitems";

	const ZBX_DC_IPMIITEM	*ipmiitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->ipmiitems, &iter);

	while (NULL != (ipmiitem = (ZBX_DC_IPMIITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, ipmiitem->itemid);
		zabbix_log(LOG_LEVEL_TRACE, "  ipmi_sensor '%s'", ipmiitem->ipmi_sensor);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_flexitems()
{
	const char		*__function_name = "DCdump_flexitems";

	const ZBX_DC_FLEXITEM	*flexitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->flexitems, &iter);

	while (NULL != (flexitem = (ZBX_DC_FLEXITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, flexitem->itemid);
		zabbix_log(LOG_LEVEL_TRACE, "  delay_flex '%s'", flexitem->delay_flex);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_trapitems()
{
	const char		*__function_name = "DCdump_trapitems";

	const ZBX_DC_TRAPITEM	*trapitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->trapitems, &iter);

	while (NULL != (trapitem = (ZBX_DC_TRAPITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, trapitem->itemid);
		zabbix_log(LOG_LEVEL_TRACE, "  trapper_hosts '%s'", trapitem->trapper_hosts);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_logitems()
{
	const char		*__function_name = "DCdump_logitems";

	const 	ZBX_DC_LOGITEM	*logitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->logitems, &iter);

	while (NULL != (logitem = (ZBX_DC_LOGITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, logitem->itemid);
		zabbix_log(LOG_LEVEL_TRACE, "  logtimefmt '%s'", logitem->logtimefmt);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_dbitems()
{
	const char		*__function_name = "DCdump_dbitems";

	const ZBX_DC_DBITEM	*dbitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->dbitems, &iter);

	while (NULL != (dbitem = (ZBX_DC_DBITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, dbitem->itemid);
		zabbix_log(LOG_LEVEL_TRACE, "  params '%s'", dbitem->params);
		zabbix_log(LOG_LEVEL_TRACE, "  username '%s'", dbitem->username);
		zabbix_log(LOG_LEVEL_TRACE, "  password '%s'", dbitem->password);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_sshitems()
{
	const char		*__function_name = "DCdump_sshitems";

	const ZBX_DC_SSHITEM	*sshitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->sshitems, &iter);

	while (NULL != (sshitem = (ZBX_DC_SSHITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, sshitem->itemid);

		zabbix_log(LOG_LEVEL_TRACE, "  username '%s'", sshitem->username);
		zabbix_log(LOG_LEVEL_TRACE, "  publickey '%s'", sshitem->publickey);
		zabbix_log(LOG_LEVEL_TRACE, "  privatekey '%s'", sshitem->privatekey);
		zabbix_log(LOG_LEVEL_TRACE, "  password '%s'", sshitem->password);
		zabbix_log(LOG_LEVEL_TRACE, "  params '%s'", sshitem->params);
		zabbix_log(LOG_LEVEL_TRACE, "  authtype %u",  sshitem->authtype);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_telnetitems()
{
	const char		*__function_name = "DCdump_telnetitems";

	const ZBX_DC_TELNETITEM	*telnetitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->telnetitems, &iter);

	while (NULL != (telnetitem = (ZBX_DC_TELNETITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, telnetitem->itemid);

		zabbix_log(LOG_LEVEL_TRACE, "  username '%s'", telnetitem->username);
		zabbix_log(LOG_LEVEL_TRACE, "  password '%s'", telnetitem->password);
		zabbix_log(LOG_LEVEL_TRACE, "  params '%s'", telnetitem->params);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_simpleitems()
{
	const char		*__function_name = "DCdump_simpleitems";

	const ZBX_DC_SIMPLEITEM	*simpleitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->simpleitems, &iter);

	while (NULL != (simpleitem = (ZBX_DC_SIMPLEITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, simpleitem->itemid);

		zabbix_log(LOG_LEVEL_TRACE, "  username '%s'", simpleitem->username);
		zabbix_log(LOG_LEVEL_TRACE, "  password '%s'", simpleitem->password);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_jmxitems()
{
	const char		*__function_name = "DCdump_jmxitems";

	const ZBX_DC_JMXITEM	*jmxitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->jmxitems, &iter);

	while (NULL != (jmxitem = (ZBX_DC_JMXITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, jmxitem->itemid);

		zabbix_log(LOG_LEVEL_TRACE, "  username '%s'", jmxitem->username);
		zabbix_log(LOG_LEVEL_TRACE, "  password '%s'", jmxitem->password);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_calcitems()
{
	const char		*__function_name = "DCdump_calcitems";

	const ZBX_DC_CALCITEM	*calcitem;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->calcitems, &iter);

	while (NULL != (calcitem = (ZBX_DC_CALCITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, calcitem->itemid);

		zabbix_log(LOG_LEVEL_TRACE, "  params '%s'", calcitem->params);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_interface_snmpitems()
{
	const char			*__function_name = "DCdump_interface_snmpitems";

	const ZBX_DC_INTERFACE_ITEM	*interface_snmpitem;
	zbx_hashset_iter_t		iter;
	int				i;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->interface_snmpitems, &iter);

	while (NULL != (interface_snmpitem = (ZBX_DC_INTERFACE_ITEM *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  interfaceid " ZBX_FS_UI64, interface_snmpitem->interfaceid);

		for (i = 0; i < interface_snmpitem->itemids.values_num; i++)
			zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, interface_snmpitem->itemids.values[i]);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_functions()
{
	const char		*__function_name = "DCdump_functions";

	const ZBX_DC_FUNCTION	*function;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->functions, &iter);

	while (NULL != (function = (ZBX_DC_FUNCTION *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  functionid " ZBX_FS_UI64, function->functionid);
		zabbix_log(LOG_LEVEL_TRACE, "  triggerid " ZBX_FS_UI64, function->triggerid);
		zabbix_log(LOG_LEVEL_TRACE, "  itemid " ZBX_FS_UI64, function->itemid);

		zabbix_log(LOG_LEVEL_TRACE, "  function '%s'", function->function);
		zabbix_log(LOG_LEVEL_TRACE, "  parameter '%s'", function->parameter);
		zabbix_log(LOG_LEVEL_TRACE, "  timer %u",  function->timer);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_trigdeps()
{
	const char			*__function_name = "DCdump_trigdeps";

	const ZBX_DC_TRIGGER_DEPLIST	*trigdep;
	zbx_hashset_iter_t		iter;
	int				i;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->trigdeps, &iter);

	while (NULL != (trigdep = (ZBX_DC_TRIGGER_DEPLIST *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  triggerid " ZBX_FS_UI64, trigdep->triggerid);

		zabbix_log(LOG_LEVEL_TRACE, "  refcount %d",  trigdep->refcount);

		for (i = 0; i < trigdep->dependencies.values_num; i++)
		{
			const ZBX_DC_TRIGGER_DEPLIST	*trigdep_up = trigdep->dependencies.values[i];

			zabbix_log(LOG_LEVEL_TRACE, "    triggerid " ZBX_FS_UI64, trigdep_up->triggerid);
			zabbix_log(LOG_LEVEL_TRACE, "    refcount %d",  trigdep_up->refcount);
		}

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_expressions()
{
	const char		*__function_name = "DCdump_expressions";

	const ZBX_DC_EXPRESSION	*expression;
	zbx_hashset_iter_t	iter;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->expressions, &iter);

	while (NULL != (expression = (ZBX_DC_EXPRESSION *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  expressionid " ZBX_FS_UI64, expression->expressionid);

		zabbix_log(LOG_LEVEL_TRACE, "  expression '%s'", expression->expression);
		zabbix_log(LOG_LEVEL_TRACE, "  regexp '%s'", expression->regexp);
		zabbix_log(LOG_LEVEL_TRACE, "  delimiter %d",  expression->delimiter);
		zabbix_log(LOG_LEVEL_TRACE, "  type %u", expression->type);
		zabbix_log(LOG_LEVEL_TRACE, "  case_sensitive %u",  expression->case_sensitive);

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_actions()
{
	const char		*__function_name = "DCdump_actions";

	const zbx_dc_action_t	*action;
	zbx_hashset_iter_t	iter;
	int			i;

	zabbix_log(LOG_LEVEL_TRACE, "  In %s()", __function_name);

	zbx_hashset_iter_reset(&config->actions, &iter);

	while (NULL != (action = (zbx_dc_action_t *)zbx_hashset_iter_next(&iter)))
	{
		zabbix_log(LOG_LEVEL_TRACE, "  actionid " ZBX_FS_UI64, action->actionid);

		zabbix_log(LOG_LEVEL_TRACE, "  formula '%s'", action->formula);
		zabbix_log(LOG_LEVEL_TRACE, "  eventsource %u",  action->eventsource);
		zabbix_log(LOG_LEVEL_TRACE, "  evaltype %u",  action->evaltype);

		for (i = 0; i < action->conditions.values_num; i++)
		{
			zbx_dc_action_condition_t	*condition = action->conditions.values[i];

			zabbix_log(LOG_LEVEL_TRACE, "    conditionid " ZBX_FS_UI64, condition->conditionid);

			zabbix_log(LOG_LEVEL_TRACE, "    conditiontype %u",  condition->conditiontype);
			zabbix_log(LOG_LEVEL_TRACE, "    operator %u",  condition->operator);
			zabbix_log(LOG_LEVEL_TRACE, "    value '%s'", condition->value);
			zabbix_log(LOG_LEVEL_TRACE, "    ====================");
		}

		zabbix_log(LOG_LEVEL_TRACE, "  ====================");
	}

	zabbix_log(LOG_LEVEL_TRACE, "  End of %s()", __function_name);
}

static void	DCdump_configuration()
{
	DCdump_config();
	DCdump_hosts();
	DCdump_proxies();
	DCdump_ipmihosts();
	DCdump_host_inventories();
	DCdump_htmpls();
	DCdump_gmacros();
	DCdump_hmacros();
	DCdump_interfaces();
	DCdump_items();
	DCdump_numitems();
	DCdump_snmpitems();
	DCdump_ipmiitems();
	DCdump_flexitems();
	DCdump_trapitems();
	DCdump_logitems();
	DCdump_dbitems();
	DCdump_sshitems();
	DCdump_telnetitems();
	DCdump_simpleitems();
	DCdump_jmxitems();
	DCdump_calcitems();
	DCdump_interface_snmpitems();
	DCdump_trigdeps();
	DCdump_functions();
	DCdump_expressions();
	DCdump_actions();
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_configuration                                             *
 *                                                                            *
 * Purpose: Synchronize configuration data from database                      *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 ******************************************************************************/
void	DCsync_configuration(unsigned char mode)
{
	const char		*__function_name = "DCsync_configuration";

	int			i, refresh_unsupported_changed;
	double			sec, csec, hsec, hisec, htsec, gmsec, hmsec, ifsec, isec, tsec, dsec, fsec, expr_sec,
				csec2, hsec2, hisec2, htsec2, gmsec2, hmsec2, ifsec2, isec2, tsec2, dsec2, fsec2,
				expr_sec2, action_sec, action_sec2, action_condition_sec, action_condition_sec2,
				total, total2, update_sec;
	const zbx_strpool_t	*strpool;

	zbx_dbsync_t		config_sync, hosts_sync, hi_sync, htmpl_sync, gmacro_sync, hmacro_sync, if_sync,
				items_sync, triggers_sync, tdep_sync, func_sync, expr_sync, action_sync,
				action_condition_sync;
	zbx_uint64_t		update_flags = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_dbsync_init(&config_sync, mode);
	zbx_dbsync_init(&hosts_sync, mode);
	zbx_dbsync_init(&hi_sync, mode);
	zbx_dbsync_init(&htmpl_sync, mode);
	zbx_dbsync_init(&gmacro_sync, mode);
	zbx_dbsync_init(&hmacro_sync, mode);
	zbx_dbsync_init(&if_sync, mode);
	zbx_dbsync_init(&items_sync, mode);
	zbx_dbsync_init(&triggers_sync, mode);
	zbx_dbsync_init(&tdep_sync, mode);
	zbx_dbsync_init(&func_sync, mode);
	zbx_dbsync_init(&expr_sync, mode);
	zbx_dbsync_init(&action_sync, mode);
	zbx_dbsync_init(&action_condition_sync, mode);

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_config(config, &config_sync))
		goto out;
	csec = zbx_time() - sec;

	/* sync global configuration settings */
	START_SYNC;
	sec = zbx_time();
	DCsync_config(&config_sync, &refresh_unsupported_changed);
	csec2 = zbx_time() - sec;
	FINISH_SYNC;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_hosts(config, &hosts_sync))
		goto out;
	hsec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_host_inventory(config, &hi_sync))
		goto out;
	hisec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_host_templates(config, &htmpl_sync))
		goto out;
	htsec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_global_macros(config, &gmacro_sync))
		goto out;
	gmsec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_host_macros(config, &hmacro_sync))
		goto out;
	hmsec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_interfaces(config, &if_sync))
		goto out;
	ifsec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_items(config, &items_sync))
		goto out;
	isec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_triggers(config, &triggers_sync))
		goto out;
	tsec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_trigger_dependency(config, &tdep_sync))
		goto out;
	dsec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_functions(config, &func_sync))
		goto out;
	fsec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_expressions(config, &expr_sync))
		goto out;
	expr_sec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_actions(config, &action_sync))
		goto out;
	action_sec = zbx_time() - sec;

	sec = zbx_time();
	if (FAIL == zbx_dbsync_compare_action_conditions(config, &action_condition_sync))
		goto out;
	action_condition_sec = zbx_time() - sec;

	START_SYNC;

	sec = zbx_time();
	DCsync_hosts(&hosts_sync);
	hsec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_host_inventory(&hi_sync);
	hisec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_htmpls(&htmpl_sync);
	htsec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_gmacros(&gmacro_sync);
	gmsec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_hmacros(&hmacro_sync);
	hmsec2 = zbx_time() - sec;

	sec = zbx_time();
	/* resolves macros for interface_snmpaddrs, must be after DCsync_hmacros() */
	DCsync_interfaces(&if_sync);
	ifsec2 = zbx_time() - sec;

	sec = zbx_time();
	/* relies on hosts, proxies and interfaces, must be after DCsync_{hosts,interfaces}() */
	DCsync_items(&items_sync, refresh_unsupported_changed);
	isec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_triggers(&triggers_sync);
	tsec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_trigdeps(&tdep_sync);
	dsec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_functions(&func_sync);
	fsec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_expressions(&expr_sync);
	expr_sec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_actions(&action_sync);
	action_sec2 = zbx_time() - sec;

	sec = zbx_time();
	DCsync_action_conditions(&action_condition_sync);
	action_condition_sec2 = zbx_time() - sec;

	sec = zbx_time();

	if (0 != hosts_sync.add_num + hosts_sync.update_num + hosts_sync.remove_num)
		update_flags |= ZBX_DBSYNC_UPDATE_HOSTS;

	if (0 != items_sync.add_num + items_sync.update_num + items_sync.remove_num)
		update_flags |= ZBX_DBSYNC_UPDATE_ITEMS;

	if (0 != htmpl_sync.add_num + htmpl_sync.update_num + htmpl_sync.remove_num)
		update_flags |= ZBX_DBSYNC_UPDATE_HOST_TEMPLATES;

	if (0 != func_sync.add_num + func_sync.update_num + func_sync.remove_num)
		update_flags |= ZBX_DBSYNC_UPDATE_FUNCTIONS;

	if (0 != gmacro_sync.add_num + gmacro_sync.update_num + gmacro_sync.remove_num)
		update_flags |= ZBX_DBSYNC_UPDATE_MACROS;

	if (0 != hmacro_sync.add_num + hmacro_sync.update_num + hmacro_sync.remove_num)
		update_flags |= ZBX_DBSYNC_UPDATE_MACROS;

	if (0 != triggers_sync.add_num + triggers_sync.update_num + triggers_sync.remove_num)
		update_flags |= ZBX_DBSYNC_UPDATE_TRIGGERS;

	if (0 != tdep_sync.add_num + tdep_sync.update_num + tdep_sync.remove_num)
		update_flags |= ZBX_DBSYNC_UPDATE_TRIGGER_DEPENDENCY;

	/* reset cached trigger expressions if there are changes in triggers or macros */
	if (0 != (update_flags & (ZBX_DBSYNC_UPDATE_MACROS | ZBX_DBSYNC_UPDATE_HOST_TEMPLATES)) ||
			0 != triggers_sync.update_num)
	{
		dc_trigger_reset_cached_expressions();
	}

	/* update trigger topology if trigger dependency was changed */
	if (0 != (update_flags & ZBX_DBSYNC_UPDATE_TRIGGER_DEPENDENCY))
		dc_trigger_update_topology();

	/* update various trigger related links in cache */
	if (0 != (update_flags & (ZBX_DBSYNC_UPDATE_HOSTS | ZBX_DBSYNC_UPDATE_ITEMS | ZBX_DBSYNC_UPDATE_FUNCTIONS |
			ZBX_DBSYNC_UPDATE_TRIGGERS)))
	{
		dc_trigger_update_cache();
	}

	update_sec = zbx_time() - sec;

	strpool = zbx_strpool_info();

	total = csec + hsec + hisec + htsec + gmsec + hmsec + ifsec + isec + tsec + dsec + fsec + expr_sec +
			action_sec + action_condition_sec;
	total2 = csec2 + hsec2 + hisec2 + htsec2 + gmsec2 + hmsec2 + ifsec2 + isec2 + tsec2 + dsec2 + fsec2 +
			expr_sec2 + action_sec2 + action_condition_sec2 + update_sec;

	zabbix_log(LOG_LEVEL_DEBUG, "%s() config     : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, csec, csec2, config_sync.add_num, config_sync.update_num,
			config_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() hosts      : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec. (%d/%d/%d)",
			__function_name, hsec, hsec2, hosts_sync.add_num, hosts_sync.update_num,
			hosts_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() host_invent: sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, hisec, hisec2, hi_sync.add_num, hi_sync.update_num,
			hi_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() templates  : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, htsec, htsec2, htmpl_sync.add_num, htmpl_sync.update_num,
			htmpl_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() globmacros : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, gmsec, gmsec2, gmacro_sync.add_num, gmacro_sync.update_num,
			gmacro_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() hostmacros : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, hmsec, hmsec2, hmacro_sync.add_num, hmacro_sync.update_num,
			hmacro_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() interfaces : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, ifsec, ifsec2, if_sync.add_num, if_sync.update_num,
			if_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() items      : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, isec, isec2, items_sync.add_num, items_sync.update_num,
			items_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() triggers   : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, tsec, tsec2, triggers_sync.add_num, triggers_sync.update_num,
			triggers_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() trigdeps   : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, dsec, dsec2, tdep_sync.add_num, tdep_sync.update_num,
			tdep_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() functions  : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, fsec, fsec2, func_sync.add_num, func_sync.update_num,
			func_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() expressions: sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, expr_sec, expr_sec2, expr_sync.add_num, expr_sync.update_num,
			expr_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() actions    : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, action_sec, action_sec2, action_sync.add_num, action_sync.update_num,
			action_sync.remove_num);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() conditions : sql:" ZBX_FS_DBL " sync:" ZBX_FS_DBL " sec (%d/%d/%d).",
			__function_name, action_condition_sec, action_condition_sec2, action_condition_sync.add_num,
			action_condition_sync.update_num, action_condition_sync.remove_num);

	zabbix_log(LOG_LEVEL_DEBUG, "%s() reindex    : " ZBX_FS_DBL " sec.", __function_name, update_sec);

	zabbix_log(LOG_LEVEL_DEBUG, "%s() total sql  : " ZBX_FS_DBL " sec.", __function_name, total);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() total sync : " ZBX_FS_DBL " sec.", __function_name, total2);

	zabbix_log(LOG_LEVEL_DEBUG, "%s() proxies    : %d (%d slots)", __function_name,
			config->proxies.num_data, config->proxies.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() hosts      : %d (%d slots)", __function_name,
			config->hosts.num_data, config->hosts.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() hosts_h    : %d (%d slots)", __function_name,
			config->hosts_h.num_data, config->hosts_h.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() hosts_p    : %d (%d slots)", __function_name,
			config->hosts_p.num_data, config->hosts_p.num_slots);
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	zabbix_log(LOG_LEVEL_DEBUG, "%s() psks       : %d (%d slots)", __function_name,
			config->psks.num_data, config->psks.num_slots);
#endif
	zabbix_log(LOG_LEVEL_DEBUG, "%s() ipmihosts  : %d (%d slots)", __function_name,
			config->ipmihosts.num_data, config->ipmihosts.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() host_invent: %d (%d slots)", __function_name,
			config->host_inventories.num_data, config->host_inventories.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() htmpls     : %d (%d slots)", __function_name,
			config->htmpls.num_data, config->htmpls.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() gmacros    : %d (%d slots)", __function_name,
			config->gmacros.num_data, config->gmacros.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() gmacros_m  : %d (%d slots)", __function_name,
			config->gmacros_m.num_data, config->gmacros_m.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() hmacros    : %d (%d slots)", __function_name,
			config->hmacros.num_data, config->hmacros.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() hmacros_hm : %d (%d slots)", __function_name,
			config->hmacros_hm.num_data, config->hmacros_hm.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() interfaces : %d (%d slots)", __function_name,
			config->interfaces.num_data, config->interfaces.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() interfac_ht: %d (%d slots)", __function_name,
			config->interfaces_ht.num_data, config->interfaces_ht.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() if_snmpitms: %d (%d slots)", __function_name,
			config->interface_snmpitems.num_data, config->interface_snmpitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() if_snmpaddr: %d (%d slots)", __function_name,
			config->interface_snmpaddrs.num_data, config->interface_snmpaddrs.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() items      : %d (%d slots)", __function_name,
			config->items.num_data, config->items.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() items_hk   : %d (%d slots)", __function_name,
			config->items_hk.num_data, config->items_hk.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() numitems   : %d (%d slots)", __function_name,
			config->numitems.num_data, config->numitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() snmpitems  : %d (%d slots)", __function_name,
			config->snmpitems.num_data, config->snmpitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() ipmiitems  : %d (%d slots)", __function_name,
			config->ipmiitems.num_data, config->ipmiitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() flexitems  : %d (%d slots)", __function_name,
			config->flexitems.num_data, config->flexitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() trapitems  : %d (%d slots)", __function_name,
			config->trapitems.num_data, config->trapitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() logitems   : %d (%d slots)", __function_name,
			config->logitems.num_data, config->logitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() dbitems    : %d (%d slots)", __function_name,
			config->dbitems.num_data, config->dbitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() sshitems   : %d (%d slots)", __function_name,
			config->sshitems.num_data, config->sshitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() telnetitems: %d (%d slots)", __function_name,
			config->telnetitems.num_data, config->telnetitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() simpleitems: %d (%d slots)", __function_name,
			config->simpleitems.num_data, config->simpleitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() jmxitems   : %d (%d slots)", __function_name,
			config->jmxitems.num_data, config->jmxitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() calcitems  : %d (%d slots)", __function_name,
			config->calcitems.num_data, config->calcitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() deltaitems : %d (%d slots)", __function_name,
			config->deltaitems.num_data, config->deltaitems.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() functions  : %d (%d slots)", __function_name,
			config->functions.num_data, config->functions.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() triggers   : %d (%d slots)", __function_name,
			config->triggers.num_data, config->triggers.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() trigdeps   : %d (%d slots)", __function_name,
			config->trigdeps.num_data, config->trigdeps.num_slots);
	for (i = 0; i < CONFIG_TIMER_FORKS; i++)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "%s() t_trigs[%d] : %d (%d allocated)", __function_name,
				i, config->time_triggers[i].values_num, config->time_triggers[i].values_alloc);
	}
	zabbix_log(LOG_LEVEL_DEBUG, "%s() expressions: %d (%d slots)", __function_name,
			config->expressions.num_data, config->expressions.num_slots);

	zabbix_log(LOG_LEVEL_DEBUG, "%s() actions    : %d (%d slots)", __function_name,
			config->actions.num_data, config->actions.num_slots);
	zabbix_log(LOG_LEVEL_DEBUG, "%s() conditions : %d (%d slots)", __function_name,
			config->action_conditions.num_data, config->action_conditions.num_slots);

	for (i = 0; ZBX_POLLER_TYPE_COUNT > i; i++)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "%s() queue[%d]   : %d (%d allocated)", __function_name,
				i, config->queues[i].elems_num, config->queues[i].elems_alloc);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "%s() pqueue     : %d (%d allocated)", __function_name,
			config->pqueue.elems_num, config->pqueue.elems_alloc);

	zabbix_log(LOG_LEVEL_DEBUG, "%s() configfree : " ZBX_FS_DBL "%%", __function_name,
			100 * ((double)config_mem->free_size / config_mem->orig_size));

	zabbix_log(LOG_LEVEL_DEBUG, "%s() strings    : %d (%d slots)", __function_name,
			strpool->hashset->num_data, strpool->hashset->num_slots);

	zabbix_log(LOG_LEVEL_DEBUG, "%s() strpoolfree: " ZBX_FS_DBL "%%", __function_name,
			100 * ((double)strpool->mem_info->free_size / strpool->mem_info->orig_size));

	zbx_mem_dump_stats(config_mem);
	zbx_mem_dump_stats(strpool->mem_info);

	FINISH_SYNC;
out:
	zbx_dbsync_clear(&config_sync);
	zbx_dbsync_clear(&hosts_sync);
	zbx_dbsync_clear(&hi_sync);
	zbx_dbsync_clear(&htmpl_sync);
	zbx_dbsync_clear(&gmacro_sync);
	zbx_dbsync_clear(&hmacro_sync);
	zbx_dbsync_clear(&if_sync);
	zbx_dbsync_clear(&items_sync);
	zbx_dbsync_clear(&triggers_sync);
	zbx_dbsync_clear(&tdep_sync);
	zbx_dbsync_clear(&func_sync);
	zbx_dbsync_clear(&expr_sync);
	zbx_dbsync_clear(&action_sync);
	zbx_dbsync_clear(&action_condition_sync);

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_TRACE))
		DCdump_configuration();

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Helper functions for configuration cache data structure element comparison *
 * and hash value calculatiif_statson.                                        *
 *                                                                            *
 * The __config_mem_XXX_func(), __config_XXX_hash and __config_XXX_compare    *
 * functions are used only inside init_configuration_cache() function to      *
 * initialize internal data structures.                                       *
 *                                                                            *
 ******************************************************************************/

static zbx_hash_t	__config_item_hk_hash(const void *data)
{
	const ZBX_DC_ITEM_HK	*item_hk = (const ZBX_DC_ITEM_HK *)data;

	zbx_hash_t		hash;

	hash = ZBX_DEFAULT_UINT64_HASH_FUNC(&item_hk->hostid);
	hash = ZBX_DEFAULT_STRING_HASH_ALGO(item_hk->key, strlen(item_hk->key), hash);

	return hash;
}

static int	__config_item_hk_compare(const void *d1, const void *d2)
{
	const ZBX_DC_ITEM_HK	*item_hk_1 = (const ZBX_DC_ITEM_HK *)d1;
	const ZBX_DC_ITEM_HK	*item_hk_2 = (const ZBX_DC_ITEM_HK *)d2;

	ZBX_RETURN_IF_NOT_EQUAL(item_hk_1->hostid, item_hk_2->hostid);

	return item_hk_1->key == item_hk_2->key ? 0 : strcmp(item_hk_1->key, item_hk_2->key);
}

static zbx_hash_t	__config_host_h_hash(const void *data)
{
	const ZBX_DC_HOST_H	*host_h = (const ZBX_DC_HOST_H *)data;

	return ZBX_DEFAULT_STRING_HASH_ALGO(host_h->host, strlen(host_h->host), ZBX_DEFAULT_HASH_SEED);
}

static int	__config_host_h_compare(const void *d1, const void *d2)
{
	const ZBX_DC_HOST_H	*host_h_1 = (const ZBX_DC_HOST_H *)d1;
	const ZBX_DC_HOST_H	*host_h_2 = (const ZBX_DC_HOST_H *)d2;

	return host_h_1->host == host_h_2->host ? 0 : strcmp(host_h_1->host, host_h_2->host);
}

static zbx_hash_t	__config_gmacro_m_hash(const void *data)
{
	const ZBX_DC_GMACRO_M	*gmacro_m = (const ZBX_DC_GMACRO_M *)data;

	zbx_hash_t		hash;

	hash = ZBX_DEFAULT_STRING_HASH_FUNC(gmacro_m->macro);

	return hash;
}

static int	__config_gmacro_m_compare(const void *d1, const void *d2)
{
	const ZBX_DC_GMACRO_M	*gmacro_m_1 = (const ZBX_DC_GMACRO_M *)d1;
	const ZBX_DC_GMACRO_M	*gmacro_m_2 = (const ZBX_DC_GMACRO_M *)d2;

	return gmacro_m_1->macro == gmacro_m_2->macro ? 0 : strcmp(gmacro_m_1->macro, gmacro_m_2->macro);
}

static zbx_hash_t	__config_hmacro_hm_hash(const void *data)
{
	const ZBX_DC_HMACRO_HM	*hmacro_hm = (const ZBX_DC_HMACRO_HM *)data;

	zbx_hash_t		hash;

	hash = ZBX_DEFAULT_UINT64_HASH_FUNC(&hmacro_hm->hostid);
	hash = ZBX_DEFAULT_STRING_HASH_ALGO(hmacro_hm->macro, strlen(hmacro_hm->macro), hash);

	return hash;
}

static int	__config_hmacro_hm_compare(const void *d1, const void *d2)
{
	const ZBX_DC_HMACRO_HM	*hmacro_hm_1 = (const ZBX_DC_HMACRO_HM *)d1;
	const ZBX_DC_HMACRO_HM	*hmacro_hm_2 = (const ZBX_DC_HMACRO_HM *)d2;

	ZBX_RETURN_IF_NOT_EQUAL(hmacro_hm_1->hostid, hmacro_hm_2->hostid);

	return hmacro_hm_1->macro == hmacro_hm_2->macro ? 0 : strcmp(hmacro_hm_1->macro, hmacro_hm_2->macro);
}

static zbx_hash_t	__config_interface_ht_hash(const void *data)
{
	const ZBX_DC_INTERFACE_HT	*interface_ht = (const ZBX_DC_INTERFACE_HT *)data;

	zbx_hash_t			hash;

	hash = ZBX_DEFAULT_UINT64_HASH_FUNC(&interface_ht->hostid);
	hash = ZBX_DEFAULT_STRING_HASH_ALGO((char *)&interface_ht->type, 1, hash);

	return hash;
}

static int	__config_interface_ht_compare(const void *d1, const void *d2)
{
	const ZBX_DC_INTERFACE_HT	*interface_ht_1 = (const ZBX_DC_INTERFACE_HT *)d1;
	const ZBX_DC_INTERFACE_HT	*interface_ht_2 = (const ZBX_DC_INTERFACE_HT *)d2;

	ZBX_RETURN_IF_NOT_EQUAL(interface_ht_1->hostid, interface_ht_2->hostid);
	ZBX_RETURN_IF_NOT_EQUAL(interface_ht_1->type, interface_ht_2->type);

	return 0;
}

static zbx_hash_t	__config_interface_addr_hash(const void *data)
{
	const ZBX_DC_INTERFACE_ADDR	*interface_addr = (const ZBX_DC_INTERFACE_ADDR *)data;

	return ZBX_DEFAULT_STRING_HASH_ALGO(interface_addr->addr, strlen(interface_addr->addr), ZBX_DEFAULT_HASH_SEED);
}

static int	__config_interface_addr_compare(const void *d1, const void *d2)
{
	const ZBX_DC_INTERFACE_ADDR	*interface_addr_1 = (const ZBX_DC_INTERFACE_ADDR *)d1;
	const ZBX_DC_INTERFACE_ADDR	*interface_addr_2 = (const ZBX_DC_INTERFACE_ADDR *)d2;

	return (interface_addr_1->addr == interface_addr_2->addr ? 0 : strcmp(interface_addr_1->addr, interface_addr_2->addr));
}

static int	__config_snmp_item_compare(const ZBX_DC_ITEM *i1, const ZBX_DC_ITEM *i2)
{
	const ZBX_DC_SNMPITEM	*s1;
	const ZBX_DC_SNMPITEM	*s2;

	unsigned char		f1;
	unsigned char		f2;

	ZBX_RETURN_IF_NOT_EQUAL(i1->interfaceid, i2->interfaceid);
	ZBX_RETURN_IF_NOT_EQUAL(i1->port, i2->port);
	ZBX_RETURN_IF_NOT_EQUAL(i1->type, i2->type);

	f1 = ZBX_FLAG_DISCOVERY_RULE & i1->flags;
	f2 = ZBX_FLAG_DISCOVERY_RULE & i2->flags;

	ZBX_RETURN_IF_NOT_EQUAL(f1, f2);

	s1 = zbx_hashset_search(&config->snmpitems, &i1->itemid);
	s2 = zbx_hashset_search(&config->snmpitems, &i2->itemid);

	ZBX_RETURN_IF_NOT_EQUAL(s1->snmp_community, s2->snmp_community);
	ZBX_RETURN_IF_NOT_EQUAL(s1->snmpv3_securityname, s2->snmpv3_securityname);
	ZBX_RETURN_IF_NOT_EQUAL(s1->snmpv3_authpassphrase, s2->snmpv3_authpassphrase);
	ZBX_RETURN_IF_NOT_EQUAL(s1->snmpv3_privpassphrase, s2->snmpv3_privpassphrase);
	ZBX_RETURN_IF_NOT_EQUAL(s1->snmpv3_contextname, s2->snmpv3_contextname);
	ZBX_RETURN_IF_NOT_EQUAL(s1->snmpv3_securitylevel, s2->snmpv3_securitylevel);
	ZBX_RETURN_IF_NOT_EQUAL(s1->snmpv3_authprotocol, s2->snmpv3_authprotocol);
	ZBX_RETURN_IF_NOT_EQUAL(s1->snmpv3_privprotocol, s2->snmpv3_privprotocol);
	ZBX_RETURN_IF_NOT_EQUAL(s1->snmp_oid_type, s2->snmp_oid_type);

	return 0;
}

static int	__config_heap_elem_compare(const void *d1, const void *d2)
{
	const zbx_binary_heap_elem_t	*e1 = (const zbx_binary_heap_elem_t *)d1;
	const zbx_binary_heap_elem_t	*e2 = (const zbx_binary_heap_elem_t *)d2;

	const ZBX_DC_ITEM		*i1 = (const ZBX_DC_ITEM *)e1->data;
	const ZBX_DC_ITEM		*i2 = (const ZBX_DC_ITEM *)e2->data;

	ZBX_RETURN_IF_NOT_EQUAL(i1->nextcheck, i2->nextcheck);
	ZBX_RETURN_IF_NOT_EQUAL(i1->unreachable, i2->unreachable);

	if (SUCCEED != is_snmp_type(i1->type))
	{
		if (SUCCEED != is_snmp_type(i2->type))
			return 0;

		return -1;
	}
	else
	{
		if (SUCCEED != is_snmp_type(i2->type))
			return +1;

		return __config_snmp_item_compare(i1, i2);
	}
}

static int	__config_pinger_elem_compare(const void *d1, const void *d2)
{
	const zbx_binary_heap_elem_t	*e1 = (const zbx_binary_heap_elem_t *)d1;
	const zbx_binary_heap_elem_t	*e2 = (const zbx_binary_heap_elem_t *)d2;

	const ZBX_DC_ITEM		*i1 = (const ZBX_DC_ITEM *)e1->data;
	const ZBX_DC_ITEM		*i2 = (const ZBX_DC_ITEM *)e2->data;

	ZBX_RETURN_IF_NOT_EQUAL(i1->nextcheck, i2->nextcheck);
	ZBX_RETURN_IF_NOT_EQUAL(i1->unreachable, i2->unreachable);
	ZBX_RETURN_IF_NOT_EQUAL(i1->interfaceid, i2->interfaceid);

	return 0;
}

static int	__config_java_item_compare(const ZBX_DC_ITEM *i1, const ZBX_DC_ITEM *i2)
{
	const ZBX_DC_JMXITEM	*j1;
	const ZBX_DC_JMXITEM	*j2;

	ZBX_RETURN_IF_NOT_EQUAL(i1->interfaceid, i2->interfaceid);

	j1 = zbx_hashset_search(&config->jmxitems, &i1->itemid);
	j2 = zbx_hashset_search(&config->jmxitems, &i2->itemid);

	ZBX_RETURN_IF_NOT_EQUAL(j1->username, j2->username);
	ZBX_RETURN_IF_NOT_EQUAL(j1->password, j2->password);

	return 0;
}

static int	__config_java_elem_compare(const void *d1, const void *d2)
{
	const zbx_binary_heap_elem_t	*e1 = (const zbx_binary_heap_elem_t *)d1;
	const zbx_binary_heap_elem_t	*e2 = (const zbx_binary_heap_elem_t *)d2;

	const ZBX_DC_ITEM		*i1 = (const ZBX_DC_ITEM *)e1->data;
	const ZBX_DC_ITEM		*i2 = (const ZBX_DC_ITEM *)e2->data;

	ZBX_RETURN_IF_NOT_EQUAL(i1->nextcheck, i2->nextcheck);
	ZBX_RETURN_IF_NOT_EQUAL(i1->unreachable, i2->unreachable);

	return __config_java_item_compare(i1, i2);
}

static int	__config_proxy_compare(const void *d1, const void *d2)
{
	const zbx_binary_heap_elem_t	*e1 = (const zbx_binary_heap_elem_t *)d1;
	const zbx_binary_heap_elem_t	*e2 = (const zbx_binary_heap_elem_t *)d2;

	const ZBX_DC_PROXY		*p1 = (const ZBX_DC_PROXY *)e1->data;
	const ZBX_DC_PROXY		*p2 = (const ZBX_DC_PROXY *)e2->data;

	int				nextcheck1, nextcheck2;

	nextcheck1 = (p1->proxy_config_nextcheck < p1->proxy_data_nextcheck) ?
			p1->proxy_config_nextcheck : p1->proxy_data_nextcheck;
	nextcheck2 = (p2->proxy_config_nextcheck < p2->proxy_data_nextcheck) ?
			p2->proxy_config_nextcheck : p2->proxy_data_nextcheck;

	ZBX_RETURN_IF_NOT_EQUAL(nextcheck1, nextcheck2);

	return 0;
}

/* hash and compare functions for expressions hashset */

static zbx_hash_t	__config_regexp_hash(const void *data)
{
	const ZBX_DC_REGEXP	*regexp = (const ZBX_DC_REGEXP *)data;

	return ZBX_DEFAULT_STRING_HASH_FUNC(regexp->name);
}

static int	__config_regexp_compare(const void *d1, const void *d2)
{
	const ZBX_DC_REGEXP	*r1 = (const ZBX_DC_REGEXP *)d1;
	const ZBX_DC_REGEXP	*r2 = (const ZBX_DC_REGEXP *)d2;

	return r1->name == r2->name ? 0 : strcmp(r1->name, r2->name);
}

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
static zbx_hash_t	__config_psk_hash(const void *data)
{
	const ZBX_DC_PSK	*psk_i = (const ZBX_DC_PSK *)data;

	return ZBX_DEFAULT_STRING_HASH_ALGO(psk_i->tls_psk_identity, strlen(psk_i->tls_psk_identity),
			ZBX_DEFAULT_HASH_SEED);
}

static int	__config_psk_compare(const void *d1, const void *d2)
{
	const ZBX_DC_PSK	*psk_1 = (const ZBX_DC_PSK *)d1;
	const ZBX_DC_PSK	*psk_2 = (const ZBX_DC_PSK *)d2;

	return psk_1->tls_psk_identity == psk_2->tls_psk_identity ? 0 : strcmp(psk_1->tls_psk_identity,
			psk_2->tls_psk_identity);
}
#endif

/******************************************************************************
 *                                                                            *
 * Function: init_configuration_cache                                         *
 *                                                                            *
 * Purpose: Allocate shared memory for configuration cache                    *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 ******************************************************************************/
void	init_configuration_cache(void)
{
	const char	*__function_name = "init_configuration_cache";

	int		i;
	key_t		shm_key;
	size_t		config_size;
	size_t		strpool_size;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() size:" ZBX_FS_UI64, __function_name, CONFIG_CONF_CACHE_SIZE);

	strpool_size = (size_t)(CONFIG_CONF_CACHE_SIZE * 0.15);
	config_size = CONFIG_CONF_CACHE_SIZE - strpool_size;

	if (-1 == (shm_key = zbx_ftok(CONFIG_FILE, ZBX_IPC_CONFIG_ID)))
	{
		zbx_error("Can't create IPC key for configuration cache");
		exit(EXIT_FAILURE);
	}

	if (FAIL == zbx_mutex_create_force(&config_lock, ZBX_MUTEX_CONFIG))
	{
		zbx_error("Unable to create mutex for configuration cache");
		exit(EXIT_FAILURE);
	}

	zbx_mem_create(&config_mem, shm_key, ZBX_NO_MUTEX, config_size, "configuration cache", "CacheSize", 0);

	config = __config_mem_malloc_func(NULL, sizeof(ZBX_DC_CONFIG) +
			CONFIG_TIMER_FORKS * sizeof(zbx_vector_ptr_t));
	config->time_triggers = (zbx_vector_ptr_t *)(config + 1);

#define CREATE_HASHSET(hashset, hashset_size)									\
														\
	CREATE_HASHSET_EXT(hashset, hashset_size, ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC)

#define CREATE_HASHSET_EXT(hashset, hashset_size, hash_func, compare_func)					\
														\
	zbx_hashset_create_ext(&hashset, hashset_size, hash_func, compare_func, NULL,				\
			__config_mem_malloc_func, __config_mem_realloc_func, __config_mem_free_func)

	CREATE_HASHSET(config->items, 100);
	CREATE_HASHSET(config->numitems, 0);
	CREATE_HASHSET(config->snmpitems, 0);
	CREATE_HASHSET(config->ipmiitems, 0);
	CREATE_HASHSET(config->flexitems, 0);
	CREATE_HASHSET(config->trapitems, 0);
	CREATE_HASHSET(config->logitems, 0);
	CREATE_HASHSET(config->dbitems, 0);
	CREATE_HASHSET(config->sshitems, 0);
	CREATE_HASHSET(config->telnetitems, 0);
	CREATE_HASHSET(config->simpleitems, 0);
	CREATE_HASHSET(config->jmxitems, 0);
	CREATE_HASHSET(config->calcitems, 0);
	CREATE_HASHSET(config->deltaitems, 0);
	CREATE_HASHSET(config->functions, 100);
	CREATE_HASHSET(config->triggers, 100);
	CREATE_HASHSET(config->trigdeps, 0);
	CREATE_HASHSET(config->hosts, 10);
	CREATE_HASHSET(config->proxies, 0);
	CREATE_HASHSET(config->host_inventories, 0);
	CREATE_HASHSET(config->ipmihosts, 0);
	CREATE_HASHSET(config->htmpls, 0);
	CREATE_HASHSET(config->gmacros, 0);
	CREATE_HASHSET(config->hmacros, 0);
	CREATE_HASHSET(config->interfaces, 10);
	CREATE_HASHSET(config->interface_snmpitems, 0);
	CREATE_HASHSET(config->expressions, 0);
	CREATE_HASHSET(config->actions, 0);
	CREATE_HASHSET(config->action_conditions, 0);

	CREATE_HASHSET_EXT(config->items_hk, 100, __config_item_hk_hash, __config_item_hk_compare);
	CREATE_HASHSET_EXT(config->hosts_h, 10, __config_host_h_hash, __config_host_h_compare);
	CREATE_HASHSET_EXT(config->hosts_p, 0, __config_host_h_hash, __config_host_h_compare);
	CREATE_HASHSET_EXT(config->gmacros_m, 0, __config_gmacro_m_hash, __config_gmacro_m_compare);
	CREATE_HASHSET_EXT(config->hmacros_hm, 0, __config_hmacro_hm_hash, __config_hmacro_hm_compare);
	CREATE_HASHSET_EXT(config->interfaces_ht, 10, __config_interface_ht_hash, __config_interface_ht_compare);
	CREATE_HASHSET_EXT(config->interface_snmpaddrs, 0, __config_interface_addr_hash, __config_interface_addr_compare);
	CREATE_HASHSET_EXT(config->regexps, 0, __config_regexp_hash, __config_regexp_compare);

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	CREATE_HASHSET_EXT(config->psks, 0, __config_psk_hash, __config_psk_compare);
#endif
	for (i = 0; i < CONFIG_TIMER_FORKS; i++)
	{
		zbx_vector_ptr_create_ext(&config->time_triggers[i],
				__config_mem_malloc_func,
				__config_mem_realloc_func,
				__config_mem_free_func);
	}

	for (i = 0; i < ZBX_POLLER_TYPE_COUNT; i++)
	{
		switch (i)
		{
			case ZBX_POLLER_TYPE_JAVA:
				zbx_binary_heap_create_ext(&config->queues[i],
						__config_java_elem_compare,
						ZBX_BINARY_HEAP_OPTION_DIRECT,
						__config_mem_malloc_func,
						__config_mem_realloc_func,
						__config_mem_free_func);
				break;
			case ZBX_POLLER_TYPE_PINGER:
				zbx_binary_heap_create_ext(&config->queues[i],
						__config_pinger_elem_compare,
						ZBX_BINARY_HEAP_OPTION_DIRECT,
						__config_mem_malloc_func,
						__config_mem_realloc_func,
						__config_mem_free_func);
				break;
			default:
				zbx_binary_heap_create_ext(&config->queues[i],
						__config_heap_elem_compare,
						ZBX_BINARY_HEAP_OPTION_DIRECT,
						__config_mem_malloc_func,
						__config_mem_realloc_func,
						__config_mem_free_func);
				break;
		}
	}

	zbx_binary_heap_create_ext(&config->pqueue,
					__config_proxy_compare,
					ZBX_BINARY_HEAP_OPTION_DIRECT,
					__config_mem_malloc_func,
					__config_mem_realloc_func,
					__config_mem_free_func);

	config->config = NULL;

	config->availability_diff_ts = 0;

#undef CREATE_HASHSET
#undef CREATE_HASHSET_EXT

	zbx_strpool_create(strpool_size);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: free_configuration_cache                                         *
 *                                                                            *
 * Purpose: Free memory allocated for configuration cache                     *
 *                                                                            *
 * Author: Alexei Vladishev, Aleksandrs Saveljevs                             *
 *                                                                            *
 ******************************************************************************/
void	free_configuration_cache(void)
{
	const char	*__function_name = "free_configuration_cache";

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	LOCK_CACHE;

	config = NULL;
	zbx_mem_destroy(config_mem);

	zbx_strpool_destroy();

	UNLOCK_CACHE;

	zbx_mutex_destroy(&config_lock);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: in_maintenance_without_data_collection                           *
 *                                                                            *
 * Parameters: maintenance_status - [IN] maintenance status                   *
 *                                       HOST_MAINTENANCE_STATUS_* flag       *
 *             maintenance_type   - [IN] maintenance type                     *
 *                                       MAINTENANCE_TYPE_* flag              *
 *             type               - [IN] item type                            *
 *                                       ITEM_TYPE_* flag                     *
 *                                                                            *
 * Return value: SUCCEED if host in maintenance without data collection       *
 *               FAIL otherwise                                               *
 *                                                                            *
 ******************************************************************************/
int	in_maintenance_without_data_collection(unsigned char maintenance_status, unsigned char maintenance_type,
		unsigned char type)
{
	if (HOST_MAINTENANCE_STATUS_ON != maintenance_status)
		return FAIL;

	if (MAINTENANCE_TYPE_NODATA != maintenance_type)
		return FAIL;

	if (ITEM_TYPE_INTERNAL == type)
		return FAIL;

	return SUCCEED;
}

static void	DCget_host(DC_HOST *dst_host, const ZBX_DC_HOST *src_host)
{
	const ZBX_DC_IPMIHOST		*ipmihost;
	const ZBX_DC_HOST_INVENTORY	*host_inventory;

	dst_host->hostid = src_host->hostid;
	dst_host->proxy_hostid = src_host->proxy_hostid;
	strscpy(dst_host->host, src_host->host);
	strscpy(dst_host->name, src_host->name);
	dst_host->maintenance_status = src_host->maintenance_status;
	dst_host->maintenance_type = src_host->maintenance_type;
	dst_host->maintenance_from = src_host->maintenance_from;
	dst_host->errors_from = src_host->errors_from;
	dst_host->available = src_host->available;
	dst_host->disable_until = src_host->disable_until;
	dst_host->snmp_errors_from = src_host->snmp_errors_from;
	dst_host->snmp_available = src_host->snmp_available;
	dst_host->snmp_disable_until = src_host->snmp_disable_until;
	dst_host->ipmi_errors_from = src_host->ipmi_errors_from;
	dst_host->ipmi_available = src_host->ipmi_available;
	dst_host->ipmi_disable_until = src_host->ipmi_disable_until;
	dst_host->jmx_errors_from = src_host->jmx_errors_from;
	dst_host->jmx_available = src_host->jmx_available;
	dst_host->jmx_disable_until = src_host->jmx_disable_until;
	dst_host->status = src_host->status;
	strscpy(dst_host->error, src_host->error);
	strscpy(dst_host->snmp_error, src_host->snmp_error);
	strscpy(dst_host->ipmi_error, src_host->ipmi_error);
	strscpy(dst_host->jmx_error, src_host->jmx_error);
	dst_host->tls_connect = src_host->tls_connect;
	dst_host->tls_accept = src_host->tls_accept;
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	strscpy(dst_host->tls_issuer, src_host->tls_issuer);
	strscpy(dst_host->tls_subject, src_host->tls_subject);

	if (NULL == src_host->tls_dc_psk)
	{
		*dst_host->tls_psk_identity = '\0';
		*dst_host->tls_psk = '\0';
	}
	else
	{
		strscpy(dst_host->tls_psk_identity, src_host->tls_dc_psk->tls_psk_identity);
		strscpy(dst_host->tls_psk, src_host->tls_dc_psk->tls_psk);
	}
#endif
	if (NULL != (ipmihost = zbx_hashset_search(&config->ipmihosts, &src_host->hostid)))
	{
		dst_host->ipmi_authtype = ipmihost->ipmi_authtype;
		dst_host->ipmi_privilege = ipmihost->ipmi_privilege;
		strscpy(dst_host->ipmi_username, ipmihost->ipmi_username);
		strscpy(dst_host->ipmi_password, ipmihost->ipmi_password);
	}
	else
	{
		dst_host->ipmi_authtype = 0;
		dst_host->ipmi_privilege = 2;
		*dst_host->ipmi_username = '\0';
		*dst_host->ipmi_password = '\0';
	}

	if (NULL != (host_inventory = zbx_hashset_search(&config->host_inventories, &src_host->hostid)))
		dst_host->inventory_mode = (char)host_inventory->inventory_mode;
	else
		dst_host->inventory_mode = HOST_INVENTORY_DISABLED;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_host_by_hostid                                             *
 *                                                                            *
 * Purpose: Locate host in configuration cache                                *
 *                                                                            *
 * Parameters: host - [OUT] pointer to DC_HOST structure                      *
 *             hostid - [IN] host ID from database                            *
 *                                                                            *
 * Return value: SUCCEED if record located and FAIL otherwise                 *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 ******************************************************************************/
int	DCget_host_by_hostid(DC_HOST *host, zbx_uint64_t hostid)
{
	int			ret = FAIL;
	const ZBX_DC_HOST	*dc_host;

	LOCK_CACHE;

	if (NULL != (dc_host = zbx_hashset_search(&config->hosts, &hostid)))
	{
		DCget_host(host, dc_host);
		ret = SUCCEED;
	}

	UNLOCK_CACHE;

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DCcheck_proxy_permissions                                        *
 *                                                                            *
 * Purpose:                                                                   *
 *     Check access rights for an active proxy and get the proxy ID           *
 *                                                                            *
 * Parameters:                                                                *
 *     host   - [IN] proxy name                                               *
 *     sock   - [IN] connection socket context                                *
 *     hostid - [OUT] proxy ID found in configuration cache                   *
 *     error  - [OUT] error message why access was denied                     *
 *                                                                            *
 * Return value:                                                              *
 *     SUCCEED - access is allowed, FAIL - access denied                      *
 *                                                                            *
 * Comments:                                                                  *
 *     Generating of error messages is done outside of configuration cache    *
 *     locking.                                                               *
 *                                                                            *
 ******************************************************************************/
int	DCcheck_proxy_permissions(const char *host, const zbx_socket_t *sock, zbx_uint64_t *hostid, char **error)
{
	const ZBX_DC_HOST	*dc_host;
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	zbx_tls_conn_attr_t	attr;

	if (ZBX_TCP_SEC_TLS_CERT == sock->connection_type)
	{
		if (SUCCEED != zbx_tls_get_attr_cert(sock, &attr))
		{
			*error = zbx_strdup(*error, "internal error: cannot get connection attributes");
			THIS_SHOULD_NEVER_HAPPEN;
			return FAIL;
		}
	}
	else if (ZBX_TCP_SEC_TLS_PSK == sock->connection_type)
	{
		if (SUCCEED != zbx_tls_get_attr_psk(sock, &attr))
		{
			*error = zbx_strdup(*error, "internal error: cannot get connection attributes");
			THIS_SHOULD_NEVER_HAPPEN;
			return FAIL;
		}
	}
	else if (ZBX_TCP_SEC_UNENCRYPTED != sock->connection_type)
	{
		*error = zbx_strdup(*error, "internal error: invalid connection type");
		THIS_SHOULD_NEVER_HAPPEN;
		return FAIL;
	}
#endif
	LOCK_CACHE;

	if (NULL == (dc_host = DCfind_proxy(host)))
	{
		UNLOCK_CACHE;
		*error = zbx_dsprintf(*error, "proxy \"%s\" not found", host);
		return FAIL;
	}

	if (HOST_STATUS_PROXY_ACTIVE != dc_host->status)
	{
		UNLOCK_CACHE;
		*error = zbx_dsprintf(*error, "proxy \"%s\" is configured in passive mode", host);
		return FAIL;
	}

	if (0 == ((unsigned int)dc_host->tls_accept & sock->connection_type))
	{
		UNLOCK_CACHE;
		*error = zbx_dsprintf(NULL, "connection of type \"%s\" is not allowed for proxy \"%s\"",
				zbx_tcp_connection_type_name(sock->connection_type), host);
		return FAIL;
	}

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	if (ZBX_TCP_SEC_TLS_CERT == sock->connection_type)
	{
		/* simplified match, not compliant with RFC 4517, 4518 */
		if ('\0' != *dc_host->tls_issuer && 0 != strcmp(dc_host->tls_issuer, attr.issuer))
		{
			UNLOCK_CACHE;
			*error = zbx_dsprintf(*error, "proxy \"%s\" certificate issuer does not match", host);
			return FAIL;
		}

		/* simplified match, not compliant with RFC 4517, 4518 */
		if ('\0' != *dc_host->tls_subject && 0 != strcmp(dc_host->tls_subject, attr.subject))
		{
			UNLOCK_CACHE;
			*error = zbx_dsprintf(*error, "proxy \"%s\" certificate subject does not match", host);
			return FAIL;
		}
	}
	else if (ZBX_TCP_SEC_TLS_PSK == sock->connection_type)
	{
		if (NULL != dc_host->tls_dc_psk)
		{
			if (strlen(dc_host->tls_dc_psk->tls_psk_identity) != attr.psk_identity_len ||
					0 != memcmp(dc_host->tls_dc_psk->tls_psk_identity, attr.psk_identity,
					attr.psk_identity_len))
			{
				UNLOCK_CACHE;
				*error = zbx_dsprintf(*error, "proxy \"%s\" is using false PSK identity", host);
				return FAIL;
			}
		}
		else
		{
			UNLOCK_CACHE;
			*error = zbx_dsprintf(*error, "active proxy \"%s\" is connecting with PSK but there is no PSK"
					" in the database for this proxy", host);
			return FAIL;
		}
	}
#endif
	*hostid = dc_host->hostid;

	UNLOCK_CACHE;

	return SUCCEED;
}

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: DCget_psk_by_identity                                            *
 *                                                                            *
 * Purpose:                                                                   *
 *     Find PSK with the specified identity in configuration cache            *
 *                                                                            *
 * Parameters:                                                                *
 *     psk_identity - [IN] PSK identity to search for ('\0' terminated)       *
 *     psk_buf      - [OUT] output buffer for PSK value                       *
 *     psk_buf_len  - [IN] output buffer size                                 *
 *                                                                            *
 * Return value:                                                              *
 *     PSK length in bytes if PSK found. 0 - if PSK not found.                *
 *                                                                            *
 * Comments:                                                                  *
 *     ATTENTION! This function's address and arguments are described and     *
 *     used in file src/libs/zbxcrypto/tls.c for calling this function by     *
 *     pointer. If you ever change this DCget_psk_by_identity() function      *
 *     arguments or return value do not forget to synchronize changes with    *
 *     the src/libs/zbxcrypto/tls.c.                                          *
 *                                                                            *
 ******************************************************************************/
size_t	DCget_psk_by_identity(const unsigned char *psk_identity, unsigned char *psk_buf, size_t psk_buf_len)
{
	const ZBX_DC_PSK	*psk_i;
	ZBX_DC_PSK		psk_i_local;
	size_t			psk_len = 0;

	LOCK_CACHE;

	psk_i_local.tls_psk_identity = (const char *)psk_identity;

	if (NULL != (psk_i = zbx_hashset_search(&config->psks, &psk_i_local)))
		psk_len = zbx_strlcpy((char *)psk_buf, psk_i->tls_psk, psk_buf_len);

	UNLOCK_CACHE;

	return psk_len;
}
#endif

static void	DCget_interface(DC_INTERFACE *dst_interface, const ZBX_DC_INTERFACE *src_interface)
{
	if (NULL != src_interface)
	{
		dst_interface->interfaceid = src_interface->interfaceid;
		strscpy(dst_interface->ip_orig, src_interface->ip);
		strscpy(dst_interface->dns_orig, src_interface->dns);
		strscpy(dst_interface->port_orig, src_interface->port);
		dst_interface->useip = src_interface->useip;
		dst_interface->type = src_interface->type;
		dst_interface->main = src_interface->main;
	}
	else
	{
		dst_interface->interfaceid = 0;
		*dst_interface->ip_orig = '\0';
		*dst_interface->dns_orig = '\0';
		*dst_interface->port_orig = '\0';
		dst_interface->useip = 1;
		dst_interface->type = INTERFACE_TYPE_UNKNOWN;
		dst_interface->main = 0;
	}

	dst_interface->addr = (1 == dst_interface->useip ? dst_interface->ip_orig : dst_interface->dns_orig);
	dst_interface->port = 0;
}

static void	DCget_item(DC_ITEM *dst_item, const ZBX_DC_ITEM *src_item)
{
	const ZBX_DC_NUMITEM		*numitem;
	const ZBX_DC_LOGITEM		*logitem;
	const ZBX_DC_SNMPITEM		*snmpitem;
	const ZBX_DC_TRAPITEM		*trapitem;
	const ZBX_DC_IPMIITEM		*ipmiitem;
	const ZBX_DC_DBITEM		*dbitem;
	const ZBX_DC_FLEXITEM		*flexitem;
	const ZBX_DC_SSHITEM		*sshitem;
	const ZBX_DC_TELNETITEM		*telnetitem;
	const ZBX_DC_SIMPLEITEM		*simpleitem;
	const ZBX_DC_JMXITEM		*jmxitem;
	const ZBX_DC_CALCITEM		*calcitem;
	const ZBX_DC_INTERFACE		*dc_interface;

	dst_item->itemid = src_item->itemid;
	dst_item->type = src_item->type;
	dst_item->data_type = src_item->data_type;
	dst_item->value_type = src_item->value_type;
	strscpy(dst_item->key_orig, src_item->key);
	dst_item->key = NULL;
	dst_item->delay = src_item->delay;
	dst_item->nextcheck = src_item->nextcheck;
	dst_item->state = src_item->state;
	dst_item->lastclock = src_item->lastclock;
	dst_item->flags = src_item->flags;
	dst_item->lastlogsize = src_item->lastlogsize;
	dst_item->mtime = src_item->mtime;
	dst_item->history = src_item->history;
	dst_item->inventory_link = src_item->inventory_link;
	dst_item->valuemapid = src_item->valuemapid;
	dst_item->status = src_item->status;
	dst_item->unreachable = src_item->unreachable;

	dst_item->db_state = src_item->db_state;
	dst_item->db_error = zbx_strdup(NULL, src_item->db_error);

	if (NULL != (flexitem = zbx_hashset_search(&config->flexitems, &src_item->itemid)))
		strscpy(dst_item->delay_flex, flexitem->delay_flex);
	else
		*dst_item->delay_flex = '\0';

	switch (src_item->value_type)
	{
		case ITEM_VALUE_TYPE_FLOAT:
		case ITEM_VALUE_TYPE_UINT64:
			numitem = zbx_hashset_search(&config->numitems, &src_item->itemid);

			dst_item->delta = numitem->delta;
			dst_item->multiplier = numitem->multiplier;
			dst_item->formula = zbx_strdup(NULL, numitem->formula);
			dst_item->trends = numitem->trends;
			dst_item->units = zbx_strdup(NULL, numitem->units);
			break;
		case ITEM_VALUE_TYPE_LOG:
			if (NULL != (logitem = zbx_hashset_search(&config->logitems, &src_item->itemid)))
				strscpy(dst_item->logtimefmt, logitem->logtimefmt);
			else
				*dst_item->logtimefmt = '\0';
			break;
	}

	switch (src_item->type)
	{
		case ITEM_TYPE_SNMPv1:
		case ITEM_TYPE_SNMPv2c:
		case ITEM_TYPE_SNMPv3:
			snmpitem = zbx_hashset_search(&config->snmpitems, &src_item->itemid);

			strscpy(dst_item->snmp_community_orig, snmpitem->snmp_community);
			strscpy(dst_item->snmp_oid_orig, snmpitem->snmp_oid);
			strscpy(dst_item->snmpv3_securityname_orig, snmpitem->snmpv3_securityname);
			dst_item->snmpv3_securitylevel = snmpitem->snmpv3_securitylevel;
			strscpy(dst_item->snmpv3_authpassphrase_orig, snmpitem->snmpv3_authpassphrase);
			strscpy(dst_item->snmpv3_privpassphrase_orig, snmpitem->snmpv3_privpassphrase);
			dst_item->snmpv3_authprotocol = snmpitem->snmpv3_authprotocol;
			dst_item->snmpv3_privprotocol = snmpitem->snmpv3_privprotocol;
			strscpy(dst_item->snmpv3_contextname_orig, snmpitem->snmpv3_contextname);

			dst_item->snmp_community = NULL;
			dst_item->snmp_oid = NULL;
			dst_item->snmpv3_securityname = NULL;
			dst_item->snmpv3_authpassphrase = NULL;
			dst_item->snmpv3_privpassphrase = NULL;
			dst_item->snmpv3_contextname = NULL;
			break;
		case ITEM_TYPE_TRAPPER:
			if (NULL != (trapitem = zbx_hashset_search(&config->trapitems, &src_item->itemid)))
				strscpy(dst_item->trapper_hosts, trapitem->trapper_hosts);
			else
				*dst_item->trapper_hosts = '\0';
			break;
		case ITEM_TYPE_IPMI:
			if (NULL != (ipmiitem = zbx_hashset_search(&config->ipmiitems, &src_item->itemid)))
				strscpy(dst_item->ipmi_sensor, ipmiitem->ipmi_sensor);
			else
				*dst_item->ipmi_sensor = '\0';
			break;
		case ITEM_TYPE_DB_MONITOR:
			if (NULL != (dbitem = zbx_hashset_search(&config->dbitems, &src_item->itemid)))
			{
				dst_item->params = zbx_strdup(NULL, dbitem->params);
				strscpy(dst_item->username_orig, dbitem->username);
				strscpy(dst_item->password_orig, dbitem->password);
			}
			else
			{
				dst_item->params = zbx_strdup(NULL, "");
				*dst_item->username_orig = '\0';
				*dst_item->password_orig = '\0';
			}
			dst_item->username = NULL;
			dst_item->password = NULL;

			break;
		case ITEM_TYPE_SSH:
			if (NULL != (sshitem = zbx_hashset_search(&config->sshitems, &src_item->itemid)))
			{
				dst_item->authtype = sshitem->authtype;
				strscpy(dst_item->username_orig, sshitem->username);
				strscpy(dst_item->publickey_orig, sshitem->publickey);
				strscpy(dst_item->privatekey_orig, sshitem->privatekey);
				strscpy(dst_item->password_orig, sshitem->password);
				dst_item->params = zbx_strdup(NULL, sshitem->params);
			}
			else
			{
				dst_item->authtype = 0;
				*dst_item->username_orig = '\0';
				*dst_item->publickey_orig = '\0';
				*dst_item->privatekey_orig = '\0';
				*dst_item->password_orig = '\0';
				dst_item->params = zbx_strdup(NULL, "");
			}
			dst_item->username = NULL;
			dst_item->publickey = NULL;
			dst_item->privatekey = NULL;
			dst_item->password = NULL;
			break;
		case ITEM_TYPE_TELNET:
			if (NULL != (telnetitem = zbx_hashset_search(&config->telnetitems, &src_item->itemid)))
			{
				strscpy(dst_item->username_orig, telnetitem->username);
				strscpy(dst_item->password_orig, telnetitem->password);
				dst_item->params = zbx_strdup(NULL, telnetitem->params);
			}
			else
			{
				*dst_item->username_orig = '\0';
				*dst_item->password_orig = '\0';
				dst_item->params = zbx_strdup(NULL, "");
			}
			dst_item->username = NULL;
			dst_item->password = NULL;
			break;
		case ITEM_TYPE_SIMPLE:
			if (NULL != (simpleitem = zbx_hashset_search(&config->simpleitems, &src_item->itemid)))
			{
				strscpy(dst_item->username_orig, simpleitem->username);
				strscpy(dst_item->password_orig, simpleitem->password);
			}
			else
			{
				*dst_item->username_orig = '\0';
				*dst_item->password_orig = '\0';
			}
			dst_item->username = NULL;
			dst_item->password = NULL;
			break;
		case ITEM_TYPE_JMX:
			if (NULL != (jmxitem = zbx_hashset_search(&config->jmxitems, &src_item->itemid)))
			{
				strscpy(dst_item->username_orig, jmxitem->username);
				strscpy(dst_item->password_orig, jmxitem->password);
			}
			else
			{
				*dst_item->username_orig = '\0';
				*dst_item->password_orig = '\0';
			}
			dst_item->username = NULL;
			dst_item->password = NULL;
			break;
		case ITEM_TYPE_CALCULATED:
			calcitem = zbx_hashset_search(&config->calcitems, &src_item->itemid);
			dst_item->params = zbx_strdup(NULL, NULL != calcitem ? calcitem->params : "");
			break;
		default:
			/* nothing to do */;
	}

	dc_interface = zbx_hashset_search(&config->interfaces, &src_item->interfaceid);

	DCget_interface(&dst_item->interface, dc_interface);

	if ('\0' != *src_item->port)
	{
		switch (src_item->type)
		{
			case ITEM_TYPE_SNMPv1:
			case ITEM_TYPE_SNMPv2c:
			case ITEM_TYPE_SNMPv3:
				strscpy(dst_item->interface.port_orig, src_item->port);
				break;
			default:
				/* nothing to do */;
		}
	}
}

void	DCconfig_clean_items(DC_ITEM *items, int *errcodes, size_t num)
{
	size_t	i;

	for (i = 0; i < num; i++)
	{
		if (NULL != errcodes && SUCCEED != errcodes[i])
			continue;

		if (ITEM_VALUE_TYPE_FLOAT == items[i].value_type || ITEM_VALUE_TYPE_UINT64 == items[i].value_type)
		{
			zbx_free(items[i].formula);
			zbx_free(items[i].units);
		}

		switch (items[i].type)
		{
			case ITEM_TYPE_DB_MONITOR:
			case ITEM_TYPE_SSH:
			case ITEM_TYPE_TELNET:
			case ITEM_TYPE_CALCULATED:
				zbx_free(items[i].params);
				break;
		}

		zbx_free(items[i].db_error);
	}
}

static void	DCget_function(DC_FUNCTION *dst_function, const ZBX_DC_FUNCTION *src_function)
{
	size_t	sz_function, sz_parameter;

	dst_function->functionid = src_function->functionid;
	dst_function->triggerid = src_function->triggerid;
	dst_function->itemid = src_function->itemid;

	sz_function = strlen(src_function->function) + 1;
	sz_parameter = strlen(src_function->parameter) + 1;
	dst_function->function = zbx_malloc(NULL, sz_function + sz_parameter);
	dst_function->parameter = dst_function->function + sz_function;
	memcpy(dst_function->function, src_function->function, sz_function);
	memcpy(dst_function->parameter, src_function->parameter, sz_parameter);
}

static void	DCget_trigger(DC_TRIGGER *dst_trigger, ZBX_DC_TRIGGER *src_trigger, unsigned char expand)
{
	dst_trigger->triggerid = src_trigger->triggerid;
	dst_trigger->description = zbx_strdup(NULL, src_trigger->description);
	dst_trigger->expression_orig = zbx_strdup(NULL, src_trigger->expression);
	dst_trigger->error = zbx_strdup(NULL, src_trigger->error);
	dst_trigger->new_error = NULL;
	dst_trigger->timespec.sec = 0;
	dst_trigger->timespec.ns = 0;
	dst_trigger->priority = src_trigger->priority;
	dst_trigger->type = src_trigger->type;
	dst_trigger->value = src_trigger->value;
	dst_trigger->state = src_trigger->state;
	dst_trigger->new_value = TRIGGER_VALUE_UNKNOWN;
	dst_trigger->lastchange = src_trigger->lastchange;
	dst_trigger->topoindex = src_trigger->topoindex;
	dst_trigger->status = src_trigger->status;

	if (ZBX_EXPAND_MACROS == expand)
	{
		dst_trigger->expression = dc_cache_expanded_expression(src_trigger->expression,
				&src_trigger->expression_ex, &dst_trigger->new_error);
	}
	else
		dst_trigger->expression = NULL;
}

static void	DCclean_trigger(DC_TRIGGER *trigger)
{
	zbx_free(trigger->new_error);
	zbx_free(trigger->error);
	zbx_free(trigger->expression_orig);
	zbx_free(trigger->expression);
	zbx_free(trigger->description);
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_items_by_keys                                       *
 *                                                                            *
 * Purpose: locate item in configuration cache by host and key                *
 *                                                                            *
 * Parameters: items    - [OUT] pointer to array of DC_ITEM structures        *
 *             keys     - [IN] list of item keys with host names              *
 *             errcodes - [OUT] SUCCEED if record located and FAIL otherwise  *
 *             num      - [IN] number of elements in items, keys, errcodes    *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_get_items_by_keys(DC_ITEM *items, zbx_host_key_t *keys, int *errcodes, size_t num)
{
	size_t			i;
	const ZBX_DC_ITEM	*dc_item;
	const ZBX_DC_HOST	*dc_host;

	LOCK_CACHE;

	for (i = 0; i < num; i++)
	{
		if (NULL == (dc_host = DCfind_host(keys[i].host)) ||
				NULL == (dc_item = DCfind_item(dc_host->hostid, keys[i].key)))
		{
			errcodes[i] = FAIL;
			continue;
		}

		DCget_host(&items[i].host, dc_host);
		DCget_item(&items[i], dc_item);
		errcodes[i] = SUCCEED;
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_items_by_itemids                                    *
 *                                                                            *
 * Purpose: Get item with specified ID                                        *
 *                                                                            *
 * Parameters: items    - [OUT] pointer to DC_ITEM structures                 *
 *             itemids  - [IN] array of item IDs                              *
 *             errcodes - [OUT] SUCCEED if item found, otherwise FAIL         *
 *             num      - [IN] number of elements                             *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_get_items_by_itemids(DC_ITEM *items, const zbx_uint64_t *itemids, int *errcodes, size_t num)
{
	size_t			i;
	const ZBX_DC_ITEM	*dc_item;
	const ZBX_DC_HOST	*dc_host;

	LOCK_CACHE;

	for (i = 0; i < num; i++)
	{
		if (NULL == (dc_item = zbx_hashset_search(&config->items, &itemids[i])) ||
				NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)))
		{
			errcodes[i] = FAIL;
			continue;
		}

		DCget_host(&items[i].host, dc_host);
		DCget_item(&items[i], dc_item);
		errcodes[i] = SUCCEED;
	}

	UNLOCK_CACHE;
}

void	DCconfig_get_triggers_by_triggerids(DC_TRIGGER *triggers, const zbx_uint64_t *triggerids, int *errcode,
		size_t num)
{
	size_t		i;
	ZBX_DC_TRIGGER	*dc_trigger;

	LOCK_CACHE;

	for (i = 0; i < num; i++)
	{
		if (NULL == (dc_trigger = zbx_hashset_search(&config->triggers, &triggerids[i])))
		{
			errcode[i] = FAIL;
			continue;
		}

		DCget_trigger(&triggers[i], dc_trigger, 0);
		errcode[i] = SUCCEED;
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_set_item_db_state                                       *
 *                                                                            *
 * Purpose: set item db_state and db_error                                    *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_set_item_db_state(zbx_uint64_t itemid, unsigned char state, const char *error)
{
	ZBX_DC_ITEM	*dc_item;

	LOCK_CACHE;

	if (NULL != (dc_item = zbx_hashset_search(&config->items, &itemid)))
	{
		dc_item->db_state = state;
		DCstrpool_replace(1, &dc_item->db_error, error);
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_functions_by_functionids                            *
 *                                                                            *
 * Purpose: Get functions by IDs                                              *
 *                                                                            *
 * Parameters: functions   - [OUT] pointer to DC_FUNCTION structures          *
 *             functionids - [IN] array of function IDs                       *
 *             errcodes    - [OUT] SUCCEED if item found, otherwise FAIL      *
 *             num         - [IN] number of elements                          *
 *                                                                            *
 * Author: Aleksandrs Saveljevs, Alexander Vladishev                          *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_get_functions_by_functionids(DC_FUNCTION *functions, zbx_uint64_t *functionids, int *errcodes, size_t num)
{
	size_t			i;
	const ZBX_DC_FUNCTION	*dc_function;

	LOCK_CACHE;

	for (i = 0; i < num; i++)
	{
		if (NULL == (dc_function = zbx_hashset_search(&config->functions, &functionids[i])))
		{
			errcodes[i] = FAIL;
			continue;
		}

		DCget_function(&functions[i], dc_function);
		errcodes[i] = SUCCEED;
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_clean_functions                                         *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_clean_functions(DC_FUNCTION *functions, int *errcodes, size_t num)
{
	size_t	i;

	for (i = 0; i < num; i++)
	{
		if (SUCCEED != errcodes[i])
			continue;

		zbx_free(functions[i].function);
	}
}

void	DCconfig_clean_triggers(DC_TRIGGER *triggers, int *errcodes, size_t num)
{
	size_t	i;

	for (i = 0; i < num; i++)
	{
		if (SUCCEED != errcodes[i])
			continue;

		DCclean_trigger(&triggers[i]);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_lock_triggers_by_history_items                          *
 *                                                                            *
 * Purpose: Lock triggers for specified items so that multiple processes do   *
 *          not process one trigger simultaneously. Otherwise, this leads to  *
 *          problems like multiple successive OK events or escalations being  *
 *          started and not cancelled, because they are not seen in parallel  *
 *          transactions.                                                     *
 *                                                                            *
 * Parameters: history_items - [IN/OUT] list of history items history syncer  *
 *                                    wishes to take for processing; on       *
 *                                    output, the item locked field is set    *
 *                                    to 0 if the corresponding item cannot   *
 *                                    be taken                                *
 *             triggerids  - [OUT] list of trigger IDs that this function has *
 *                                 locked for processing; unlock those using  *
 *                                 DCconfig_unlock_triggers() function        *
 *                                                                            *
 * Author: Aleksandrs Saveljevs                                               *
 *                                                                            *
 * Comments: This does not solve the problem fully (e.g., ZBX-7484). There is *
 *           a significant time period between the place where we lock the    *
 *           triggers and the place where we process them. So it could happen *
 *           that a configuration cache update happens after we have locked   *
 *           the triggers and it turns out that in the updated configuration  *
 *           there is a new trigger for two of the items that two different   *
 *           history syncers have taken for processing. In that situation,    *
 *           the problem we are solving here might still happen. However,     *
 *           locking triggers makes this problem much less likely and only in *
 *           case configuration changes. On a stable configuration, it should *
 *           work without any problems.                                       *
 *                                                                            *
 *           Also see function DCconfig_get_time_based_triggers(), which      *
 *           timer processes use to lock and unlock triggers.                 *
 *                                                                            *
 * Return value: the number of items available for processing (unlocked).     *
 *                                                                            *
 ******************************************************************************/
int	DCconfig_lock_triggers_by_history_items(zbx_vector_ptr_t *history_items, zbx_vector_uint64_t *triggerids)
{
	int			i, j, locked_num = 0;
	const ZBX_DC_ITEM	*dc_item;
	ZBX_DC_TRIGGER		*dc_trigger;
	zbx_hc_item_t		*history_item;

	LOCK_CACHE;

	for (i = 0; i < history_items->values_num; i++)
	{
		history_item = (zbx_hc_item_t *)history_items->values[i];

		if (NULL == (dc_item = zbx_hashset_search(&config->items, &history_item->itemid)))
			continue;

		if (NULL == dc_item->triggers)
			continue;

		for (j = 0; NULL != (dc_trigger = dc_item->triggers[j]); j++)
		{
			if (TRIGGER_STATUS_ENABLED != dc_trigger->status)
				continue;

			if (1 == dc_trigger->locked)
			{
				locked_num++;
				history_item->status = ZBX_HC_ITEM_STATUS_BUSY;
				goto next;
			}
		}

		for (j = 0; NULL != (dc_trigger = dc_item->triggers[j]); j++)
		{
			if (TRIGGER_STATUS_ENABLED != dc_trigger->status)
				continue;

			dc_trigger->locked = 1;
			zbx_vector_uint64_append(triggerids, dc_trigger->triggerid);
		}
next:;
	}

	UNLOCK_CACHE;

	return history_items->values_num - locked_num;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_unlock_triggers                                         *
 *                                                                            *
 * Author: Aleksandrs Saveljevs                                               *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_unlock_triggers(const zbx_vector_uint64_t *triggerids)
{
	int		i;
	ZBX_DC_TRIGGER	*dc_trigger;

	LOCK_CACHE;

	for (i = 0; i < triggerids->values_num; i++)
	{
		if (NULL == (dc_trigger = zbx_hashset_search(&config->triggers, &triggerids->values[i])))
			continue;

		dc_trigger->locked = 0;
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_unlock_all_triggers                                     *
 *                                                                            *
 * Purpose: Unlocks all locked triggers before doing full history sync at     *
 *          program exit                                                      *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_unlock_all_triggers()
{
	ZBX_DC_TRIGGER		*dc_trigger;
	zbx_hashset_iter_t	iter;

	LOCK_CACHE;

	zbx_hashset_iter_reset(&config->triggers, &iter);

	while (NULL != (dc_trigger = zbx_hashset_iter_next(&iter)))
		dc_trigger->locked = 0;

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_triggers_by_itemids                                 *
 *                                                                            *
 * Purpose: get enabled triggers for specified items                          *
 *                                                                            *
 * Author: Aleksandrs Saveljevs                                               *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_get_triggers_by_itemids(zbx_hashset_t *trigger_info, zbx_vector_ptr_t *trigger_order,
		const zbx_uint64_t *itemids, const zbx_timespec_t *timespecs, char **errors, int itemids_num,
		unsigned char expand)
{
	int			i, j, found;
	const ZBX_DC_ITEM	*dc_item;
	ZBX_DC_TRIGGER		*dc_trigger;
	DC_TRIGGER		*trigger;

	LOCK_CACHE;

	for (i = 0; i < itemids_num; i++)
	{
		if (NULL == (dc_item = zbx_hashset_search(&config->items, &itemids[i])) || NULL == dc_item->triggers)
			continue;

		for (j = 0; NULL != (dc_trigger = dc_item->triggers[j]); j++)
		{
			if (TRIGGER_STATUS_ENABLED != dc_trigger->status)
				continue;

			trigger = DCfind_id(trigger_info, dc_trigger->triggerid, sizeof(DC_TRIGGER), &found);

			if (0 == found)
			{
				DCget_trigger(trigger, dc_trigger, expand);
				zbx_vector_ptr_append(trigger_order, trigger);
			}

			if (trigger->timespec.sec < timespecs[i].sec ||
					(trigger->timespec.sec == timespecs[i].sec &&
					trigger->timespec.ns < timespecs[i].ns))
			{
				trigger->timespec = timespecs[i];

				if (NULL != errors)
					trigger->new_error = zbx_strdup(trigger->new_error, errors[i]);
			}
		}
	}

	UNLOCK_CACHE;

	zbx_vector_ptr_sort(trigger_order, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_time_based_triggers                                 *
 *                                                                            *
 * Purpose: get triggers that have time-based functions (sorted by triggerid) *
 *                                                                            *
 * Author: Aleksandrs Saveljevs                                               *
 *                                                                            *
 * Comments: A trigger should have at least one function that is time-based   *
 *           and which does not have its host in no-data maintenance.         *
 *                                                                            *
 *           This function is meant to be called multiple times, each time    *
 *           yielding up to max_triggers in return. When called, the function *
 *           first unlocks triggers locked the previous time (if any), then   *
 *           starts where it left off to yield the next bunch of triggers.    *
 *                                                                            *
 *           Also see function DCconfig_lock_triggers_by_history_items(),     *
 *           which history syncer processes use to lock triggers.             *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_get_time_based_triggers(DC_TRIGGER **trigger_info, zbx_vector_ptr_t *trigger_order, int max_triggers,
		int process_num)
{
	int			i, j, lo, hi, found;
	zbx_uint64_t		functionid;
	const ZBX_DC_ITEM	*dc_item;
	const ZBX_DC_FUNCTION	*dc_function;
	ZBX_DC_TRIGGER		*dc_trigger;
	const ZBX_DC_HOST	*dc_host;
	DC_TRIGGER		*trigger;
	const char		*p, *q;

	LOCK_CACHE;

	if (0 == trigger_order->values_num)
	{
		*trigger_info = zbx_malloc(*trigger_info, max_triggers * sizeof(DC_TRIGGER));
		zbx_vector_ptr_reserve(trigger_order, max_triggers);

		hi = 0;
	}
	else
	{
		zbx_uint64_t	last_triggerid;

		trigger = (DC_TRIGGER *)trigger_order->values[0];
		lo = zbx_vector_ptr_nearestindex(&config->time_triggers[process_num - 1], &trigger->triggerid,
				ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

		trigger = (DC_TRIGGER *)trigger_order->values[trigger_order->values_num - 1];
		last_triggerid = trigger->triggerid + 1;
		hi = zbx_vector_ptr_nearestindex(&config->time_triggers[process_num - 1], &last_triggerid,
				ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

		for (i = 0, j = lo; i < trigger_order->values_num; i++)
		{
			trigger = (DC_TRIGGER *)trigger_order->values[i];

			while (j < hi)
			{
				dc_trigger = (ZBX_DC_TRIGGER *)config->time_triggers[process_num - 1].values[j];

				if (dc_trigger->triggerid >= trigger->triggerid)
					break;

				j++;
			}

			if (j < hi && dc_trigger->triggerid == trigger->triggerid)
				dc_trigger->locked = 0;
		}

		DCfree_triggers(trigger_order);
	}

	for (i = hi; i < config->time_triggers[process_num - 1].values_num; i++)
	{
		dc_trigger = (ZBX_DC_TRIGGER *)config->time_triggers[process_num - 1].values[i];

		if (TRIGGER_STATUS_ENABLED != dc_trigger->status || 1 == dc_trigger->locked)
			continue;

		found = 0;

		for (p = dc_trigger->expression; '\0' != *p; p++)
		{
			if ('{' != *p)
				continue;

			if ('$' == p[1])
			{
				int	macro_r, context_l, context_r;

				if (SUCCEED == zbx_user_macro_parse(p, &macro_r, &context_l, &context_r))
					p += macro_r;
				else
					p++;

				continue;
			}

			for (q = p + 1; '}' != *q && '\0' != *q; q++)
			{
				if ('0' > *q || '9' < *q)
					break;
			}

			if ('}' != *q)
				continue;

			sscanf(p + 1, ZBX_FS_UI64, &functionid);

			if (NULL != (dc_function = zbx_hashset_search(&config->functions, &functionid)) &&
					SUCCEED == is_time_function(dc_function->function) &&
					NULL != (dc_item = zbx_hashset_search(&config->items, &dc_function->itemid)) &&
					ITEM_STATUS_ACTIVE == dc_item->status &&
					NULL != (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)) &&
					HOST_STATUS_MONITORED == dc_host->status &&
					SUCCEED != DCin_maintenance_without_data_collection(dc_host, dc_item))
			{
				found = 1;
				break;
			}

			p = q;
		}

		if (1 == found)
		{
			dc_trigger->locked = 1;

			trigger = &(*trigger_info)[trigger_order->values_num];

			DCget_trigger(trigger, dc_trigger, ZBX_EXPAND_MACROS);
			zbx_timespec(&trigger->timespec);

			zbx_vector_ptr_append(trigger_order, trigger);

			if (trigger_order->values_num == max_triggers)
				break;
		}
	}

	UNLOCK_CACHE;
}

void	DCfree_triggers(zbx_vector_ptr_t *triggers)
{
	int	i;

	for (i = 0; i < triggers->values_num; i++)
		DCclean_trigger((DC_TRIGGER *)triggers->values[i]);

	zbx_vector_ptr_clear(triggers);
}

void	DCconfig_update_interface_snmp_stats(zbx_uint64_t interfaceid, int max_snmp_succeed, int min_snmp_fail)
{
	ZBX_DC_INTERFACE	*dc_interface;

	LOCK_CACHE;

	if (NULL != (dc_interface = zbx_hashset_search(&config->interfaces, &interfaceid)) &&
			SNMP_BULK_ENABLED == dc_interface->bulk)
	{
		if (dc_interface->max_snmp_succeed < max_snmp_succeed)
			dc_interface->max_snmp_succeed = (unsigned char)max_snmp_succeed;

		if (dc_interface->min_snmp_fail > min_snmp_fail)
			dc_interface->min_snmp_fail = (unsigned char)min_snmp_fail;
	}

	UNLOCK_CACHE;
}

static int	DCconfig_get_suggested_snmp_vars_nolock(zbx_uint64_t interfaceid, int *bulk)
{
	int			num;
	ZBX_DC_INTERFACE	*dc_interface;

	dc_interface = zbx_hashset_search(&config->interfaces, &interfaceid);

	if (NULL != bulk)
		*bulk = (NULL == dc_interface ? SNMP_BULK_DISABLED : dc_interface->bulk);

	if (NULL == dc_interface || SNMP_BULK_ENABLED != dc_interface->bulk)
		return 1;

	/* The general strategy is to multiply request size by 3/2 in order to approach the limit faster. */
	/* However, once we are over the limit, we change the strategy to increasing the value by 1. This */
	/* is deemed better than going backwards from the error because less timeouts are going to occur. */

	if (1 >= dc_interface->max_snmp_succeed || MAX_SNMP_ITEMS + 1 != dc_interface->min_snmp_fail)
		num = dc_interface->max_snmp_succeed + 1;
	else
		num = dc_interface->max_snmp_succeed * 3 / 2;

	if (num < dc_interface->min_snmp_fail)
		return num;

	/* If we have already found the optimal number of variables to query, we wish to base our suggestion on that */
	/* number. If we occasionally get a timeout in this area, it can mean two things: either the device's actual */
	/* limit is a bit lower than that (it can process requests above it, but only sometimes) or a UDP packet in  */
	/* one of the directions was lost. In order to account for the former, we allow ourselves to lower the count */
	/* of variables, but only up to two times. Otherwise, performance will gradually degrade due to the latter.  */

	return MAX(dc_interface->max_snmp_succeed - 2, dc_interface->min_snmp_fail - 1);
}

int	DCconfig_get_suggested_snmp_vars(zbx_uint64_t interfaceid, int *bulk)
{
	int	ret;

	LOCK_CACHE;

	ret = DCconfig_get_suggested_snmp_vars_nolock(interfaceid, bulk);

	UNLOCK_CACHE;

	return ret;
}

static int	dc_get_interface_by_type(DC_INTERFACE *interface, zbx_uint64_t hostid, unsigned char type)
{
	int			res = FAIL;
	const ZBX_DC_INTERFACE	*dc_interface;
	ZBX_DC_INTERFACE_HT	*interface_ht, interface_ht_local;

	interface_ht_local.hostid = hostid;
	interface_ht_local.type = type;

	if (NULL != (interface_ht = zbx_hashset_search(&config->interfaces_ht, &interface_ht_local)))
	{
		dc_interface = interface_ht->interface_ptr;
		DCget_interface(interface, dc_interface);
		res = SUCCEED;
	}

	return res;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_interface_by_type                                   *
 *                                                                            *
 * Purpose: Locate main interface of specified type in configuration cache    *
 *                                                                            *
 * Parameters: interface - [OUT] pointer to DC_INTERFACE structure            *
 *             hostid - [IN] host ID                                          *
 *             type - [IN] interface type                                     *
 *                                                                            *
 * Return value: SUCCEED if record located and FAIL otherwise                 *
 *                                                                            *
 ******************************************************************************/
int	DCconfig_get_interface_by_type(DC_INTERFACE *interface, zbx_uint64_t hostid, unsigned char type)
{
	int	res;

	LOCK_CACHE;

	res = dc_get_interface_by_type(interface, hostid, type);

	UNLOCK_CACHE;

	return res;
}


/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_interface                                           *
 *                                                                            *
 * Purpose: Locate interface in configuration cache                           *
 *                                                                            *
 * Parameters: interface - [OUT] pointer to DC_INTERFACE structure            *
 *             hostid - [IN] host ID                                          *
 *             itemid - [IN] item ID                                          *
 *                                                                            *
 * Return value: SUCCEED if record located and FAIL otherwise                 *
 *                                                                            *
 ******************************************************************************/
int	DCconfig_get_interface(DC_INTERFACE *interface, zbx_uint64_t hostid, zbx_uint64_t itemid)
{
	int			res = FAIL, i;
	ZBX_DC_ITEM		*dc_item;
	const ZBX_DC_INTERFACE	*dc_interface;

	LOCK_CACHE;

	if (0 != itemid)
	{
		if (NULL == (dc_item = zbx_hashset_search(&config->items, &itemid)))
			goto unlock;

		if (0 != dc_item->interfaceid)
		{
			if (NULL == (dc_interface = zbx_hashset_search(&config->interfaces, &dc_item->interfaceid)))
				goto unlock;

			DCget_interface(interface, dc_interface);
			res = SUCCEED;
			goto unlock;
		}

		hostid = dc_item->hostid;
	}

	if (0 == hostid)
		goto unlock;

	for (i = 0; i < (int)ARRSIZE(INTERFACE_TYPE_PRIORITY); i++)
	{
		if (SUCCEED == (res = dc_get_interface_by_type(interface, hostid, INTERFACE_TYPE_PRIORITY[i])))
			break;
	}

unlock:
	UNLOCK_CACHE;

	return res;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_config_get_queue_nextcheck                                    *
 *                                                                            *
 * Purpose: Get nextcheck for selected queue                                  *
 *                                                                            *
 * Parameters: queue - [IN] the queue                                         *
 *                                                                            *
 * Return value: nextcheck or FAIL if no items for the specified queue        *
 *                                                                            *
 ******************************************************************************/
static int	dc_config_get_queue_nextcheck(zbx_binary_heap_t *queue)
{
	int				nextcheck;
	const zbx_binary_heap_elem_t	*min;
	const ZBX_DC_ITEM		*dc_item;

	if (FAIL == zbx_binary_heap_empty(queue))
	{
		min = zbx_binary_heap_find_min(queue);
		dc_item = (const ZBX_DC_ITEM *)min->data;

		nextcheck = dc_item->nextcheck;
	}
	else
		nextcheck = FAIL;

	return nextcheck;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_poller_nextcheck                                    *
 *                                                                            *
 * Purpose: Get nextcheck for selected poller                                 *
 *                                                                            *
 * Parameters: poller_type - [IN] poller type (ZBX_POLLER_TYPE_...)           *
 *                                                                            *
 * Return value: nextcheck or FAIL if no items for selected poller            *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 ******************************************************************************/
int	DCconfig_get_poller_nextcheck(unsigned char poller_type)
{
	const char		*__function_name = "DCconfig_get_poller_nextcheck";

	int			nextcheck;
	zbx_binary_heap_t	*queue;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() poller_type:%d", __function_name, (int)poller_type);

	queue = &config->queues[poller_type];

	LOCK_CACHE;

	nextcheck = dc_config_get_queue_nextcheck(queue);

	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%d", __function_name, nextcheck);

	return nextcheck;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_poller_items                                        *
 *                                                                            *
 * Purpose: Get array of items for selected poller                            *
 *                                                                            *
 * Parameters: poller_type - [IN] poller type (ZBX_POLLER_TYPE_...)           *
 *             items       - [OUT] array of items                             *
 *                                                                            *
 * Return value: number of items in items array                               *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 * Comments: Items leave the queue only through this function. Pollers must   *
 *           always return the items they have taken using DCrequeue_items()  *
 *           or DCpoller_requeue_items().                                     *
 *                                                                            *
 *           Currently batch polling is supported only for JMX, SNMP and      *
 *           icmpping* simple checks. In other cases only single item is      *
 *           retrieved.                                                       *
 *                                                                            *
 ******************************************************************************/
int	DCconfig_get_poller_items(unsigned char poller_type, DC_ITEM *items)
{
	const char		*__function_name = "DCconfig_get_poller_items";

	int			now, num = 0, max_items;
	zbx_binary_heap_t	*queue;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() poller_type:%d", __function_name, (int)poller_type);

	now = time(NULL);

	queue = &config->queues[poller_type];

	switch (poller_type)
	{
		case ZBX_POLLER_TYPE_JAVA:
			max_items = MAX_JAVA_ITEMS;
			break;
		case ZBX_POLLER_TYPE_PINGER:
			max_items = MAX_PINGER_ITEMS;
			break;
		default:
			max_items = 1;
	}

	LOCK_CACHE;

	while (num < max_items && FAIL == zbx_binary_heap_empty(queue))
	{
		int				disable_until, old_nextcheck;
		unsigned char			old_poller_type;
		const zbx_binary_heap_elem_t	*min;
		ZBX_DC_HOST			*dc_host;
		ZBX_DC_ITEM			*dc_item;
		static const ZBX_DC_ITEM	*dc_item_prev = NULL;

		min = zbx_binary_heap_find_min(queue);
		dc_item = (ZBX_DC_ITEM *)min->data;

		if (dc_item->nextcheck > now)
			break;

		if (0 != num)
		{
			if (SUCCEED == is_snmp_type(dc_item_prev->type))
			{
				if (0 != __config_snmp_item_compare(dc_item_prev, dc_item))
					break;
			}
			else if (ITEM_TYPE_JMX == dc_item_prev->type)
			{
				if (0 != __config_java_item_compare(dc_item_prev, dc_item))
					break;
			}
		}

		zbx_binary_heap_remove_min(queue);
		dc_item->location = ZBX_LOC_NOWHERE;

		if (0 == config->config->refresh_unsupported && ITEM_STATE_NOTSUPPORTED == dc_item->state)
			continue;

		if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)))
			continue;

		if (HOST_STATUS_MONITORED != dc_host->status)
			continue;

		if (SUCCEED == DCin_maintenance_without_data_collection(dc_host, dc_item))
		{
			old_nextcheck = dc_item->nextcheck;
			dc_item->nextcheck = DCget_reachable_nextcheck(dc_item, dc_host, now);

			DCupdate_item_queue(dc_item, dc_item->poller_type, old_nextcheck);
			continue;
		}

		if (0 == (disable_until = DCget_disable_until(dc_item, dc_host)))
		{
			/* move reachable items on reachable hosts to normal pollers */
			if (ZBX_POLLER_TYPE_UNREACHABLE == poller_type && 0 == dc_item->unreachable)
			{
				old_poller_type = dc_item->poller_type;
				dc_item->poller_type = poller_by_item(dc_host->proxy_hostid, dc_item->type,
						dc_item->key);

				old_nextcheck = dc_item->nextcheck;
				dc_item->nextcheck = DCget_reachable_nextcheck(dc_item, dc_host, now);

				DCupdate_item_queue(dc_item, old_poller_type, old_nextcheck);
				continue;
			}
		}
		else
		{
			if (ZBX_POLLER_TYPE_NORMAL == poller_type ||
					ZBX_POLLER_TYPE_IPMI == poller_type ||
					ZBX_POLLER_TYPE_JAVA == poller_type)
			{
				old_poller_type = dc_item->poller_type;
				dc_item->poller_type = ZBX_POLLER_TYPE_UNREACHABLE;

				old_nextcheck = dc_item->nextcheck;
				if (disable_until > now)
					dc_item->nextcheck = DCget_unreachable_nextcheck(dc_item, dc_host);

				DCupdate_item_queue(dc_item, old_poller_type, old_nextcheck);
				continue;
			}
			else if (disable_until > now)
			{
				old_nextcheck = dc_item->nextcheck;
				dc_item->nextcheck = DCget_unreachable_nextcheck(dc_item, dc_host);

				DCupdate_item_queue(dc_item, dc_item->poller_type, old_nextcheck);
				continue;
			}

			DCincrease_disable_until(dc_item, dc_host, now);
		}

		dc_item_prev = dc_item;
		dc_item->location = ZBX_LOC_POLLER;
		DCget_host(&items[num].host, dc_host);
		DCget_item(&items[num], dc_item);
		num++;

		if (1 == num && ZBX_POLLER_TYPE_NORMAL == poller_type && SUCCEED == is_snmp_type(dc_item->type) &&
				0 == (ZBX_FLAG_DISCOVERY_RULE & dc_item->flags))
		{
			ZBX_DC_SNMPITEM	*snmpitem;

			snmpitem = zbx_hashset_search(&config->snmpitems, &dc_item->itemid);

			if (ZBX_SNMP_OID_TYPE_NORMAL == snmpitem->snmp_oid_type ||
					ZBX_SNMP_OID_TYPE_DYNAMIC == snmpitem->snmp_oid_type)
			{
				max_items = DCconfig_get_suggested_snmp_vars_nolock(dc_item->interfaceid, NULL);
			}
		}
	}

	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%d", __function_name, num);

	return num;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_snmp_interfaceids_by_addr                           *
 *                                                                            *
 * Purpose: get array of interface IDs for the specified address              *
 *                                                                            *
 * Return value: number of interface IDs returned                             *
 *                                                                            *
 * Author: Rudolfs Kreicbergs                                                 *
 *                                                                            *
 ******************************************************************************/
int	DCconfig_get_snmp_interfaceids_by_addr(const char *addr, zbx_uint64_t **interfaceids)
{
	const char		*__function_name = "DCconfig_get_snmp_interfaceids_by_addr";

	int			count = 0, i;
	ZBX_DC_INTERFACE_ADDR	*dc_interface_snmpaddr, dc_interface_snmpaddr_local;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() addr:'%s'", __function_name, addr);

	dc_interface_snmpaddr_local.addr = addr;

	LOCK_CACHE;

	if (NULL == (dc_interface_snmpaddr = zbx_hashset_search(&config->interface_snmpaddrs, &dc_interface_snmpaddr_local)))
		goto unlock;

	*interfaceids = zbx_malloc(*interfaceids, dc_interface_snmpaddr->interfaceids.values_num * sizeof(zbx_uint64_t));

	for (i = 0; i < dc_interface_snmpaddr->interfaceids.values_num; i++)
		(*interfaceids)[i] = dc_interface_snmpaddr->interfaceids.values[i];

	count = i;
unlock:
	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%d", __function_name, count);

	return count;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_snmp_items_by_interfaceid                           *
 *                                                                            *
 * Purpose: get array of snmp trap items for the specified interfaceid        *
 *                                                                            *
 * Return value: number of items returned                                     *
 *                                                                            *
 * Author: Rudolfs Kreicbergs                                                 *
 *                                                                            *
 ******************************************************************************/
size_t	DCconfig_get_snmp_items_by_interfaceid(zbx_uint64_t interfaceid, DC_ITEM **items)
{
	const char		*__function_name = "DCconfig_get_snmp_items_by_interface";

	size_t			items_num = 0, items_alloc = 8;
	int			i;
	ZBX_DC_ITEM		*dc_item;
	ZBX_DC_INTERFACE_ITEM	*dc_interface_snmpitem;
	ZBX_DC_INTERFACE	*dc_interface;
	ZBX_DC_HOST		*dc_host;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() interfaceid:" ZBX_FS_UI64, __function_name, interfaceid);

	LOCK_CACHE;

	if (NULL == (dc_interface = zbx_hashset_search(&config->interfaces, &interfaceid)))
		goto unlock;

	if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_interface->hostid)))
		goto unlock;

	if (HOST_STATUS_MONITORED != dc_host->status)
		goto unlock;

	if (NULL == (dc_interface_snmpitem = zbx_hashset_search(&config->interface_snmpitems, &interfaceid)))
		goto unlock;

	*items = zbx_malloc(*items, items_alloc * sizeof(DC_ITEM));

	for (i = 0; i < dc_interface_snmpitem->itemids.values_num; i++)
	{
		if (NULL == (dc_item = zbx_hashset_search(&config->items, &dc_interface_snmpitem->itemids.values[i])))
			continue;

		if (ITEM_STATUS_ACTIVE != dc_item->status)
			continue;

		if (SUCCEED == DCin_maintenance_without_data_collection(dc_host, dc_item))
			continue;

		if (0 == config->config->refresh_unsupported && ITEM_STATE_NOTSUPPORTED == dc_item->state)
			continue;

		if (items_num == items_alloc)
		{
			items_alloc += 8;
			*items = zbx_realloc(*items, items_alloc * sizeof(DC_ITEM));
		}

		DCget_host(&(*items)[items_num].host, dc_host);
		DCget_item(&(*items)[items_num], dc_item);
		items_num++;
	}
unlock:
	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():" ZBX_FS_SIZE_T, __function_name, (zbx_fs_size_t)items_num);

	return items_num;
}

static void	DCrequeue_reachable_item(ZBX_DC_ITEM *dc_item, const ZBX_DC_HOST *dc_host, int lastclock)
{
	unsigned char	old_poller_type;
	int		old_nextcheck;

	old_nextcheck = dc_item->nextcheck;
	dc_item->nextcheck = DCget_reachable_nextcheck(dc_item, dc_host, lastclock);

	dc_item->unreachable = 0;

	if (ZBX_NO_POLLER == dc_item->poller_type)
		return;

	old_poller_type = dc_item->poller_type;

	if (ZBX_POLLER_TYPE_UNREACHABLE == dc_item->poller_type)
		dc_item->poller_type = poller_by_item(dc_host->proxy_hostid, dc_item->type, dc_item->key);

	DCupdate_item_queue(dc_item, old_poller_type, old_nextcheck);
}

static void	DCrequeue_unreachable_item(ZBX_DC_ITEM *dc_item, const ZBX_DC_HOST *dc_host)
{
	unsigned char	old_poller_type;
	int		old_nextcheck;

	old_nextcheck = dc_item->nextcheck;
	dc_item->nextcheck = DCget_unreachable_nextcheck(dc_item, dc_host);

	dc_item->unreachable = 1;

	if (ZBX_NO_POLLER == dc_item->poller_type)
		return;

	old_poller_type = dc_item->poller_type;

	if (ZBX_POLLER_TYPE_NORMAL == dc_item->poller_type ||
			ZBX_POLLER_TYPE_IPMI == dc_item->poller_type ||
			ZBX_POLLER_TYPE_JAVA == dc_item->poller_type)
	{
		dc_item->poller_type = ZBX_POLLER_TYPE_UNREACHABLE;
	}

	DCupdate_item_queue(dc_item, old_poller_type, old_nextcheck);
}

static void	dc_requeue_items(zbx_uint64_t *itemids, unsigned char *states, int *lastclocks,
		zbx_uint64_t *lastlogsizes, int *mtimes, int *errcodes, size_t num)
{
	size_t		i;
	ZBX_DC_ITEM	*dc_item;
	ZBX_DC_HOST	*dc_host;

	for (i = 0; i < num; i++)
	{
		if (FAIL == errcodes[i])
			continue;

		if (NULL == (dc_item = zbx_hashset_search(&config->items, &itemids[i])))
			continue;

		if (ZBX_LOC_POLLER == dc_item->location)
			dc_item->location = ZBX_LOC_NOWHERE;

		if (ITEM_STATUS_ACTIVE != dc_item->status)
			continue;

		if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)))
			continue;

		if (HOST_STATUS_MONITORED != dc_host->status)
			continue;

		dc_item->state = states[i];
		dc_item->lastclock = lastclocks[i];
		if (NULL != lastlogsizes)
			dc_item->lastlogsize = lastlogsizes[i];
		if (NULL != mtimes)
			dc_item->mtime = mtimes[i];

		if (SUCCEED != is_counted_in_item_queue(dc_item->type, dc_item->key))
			continue;

		switch (errcodes[i])
		{
			case SUCCEED:
			case NOTSUPPORTED:
			case AGENT_ERROR:
			case CONFIG_ERROR:
				DCrequeue_reachable_item(dc_item, dc_host, lastclocks[i]);
				break;
			case NETWORK_ERROR:
			case GATEWAY_ERROR:
			case TIMEOUT_ERROR:
				DCrequeue_unreachable_item(dc_item, dc_host);
				break;
			default:
				THIS_SHOULD_NEVER_HAPPEN;
		}
	}
}

void	DCrequeue_items(zbx_uint64_t *itemids, unsigned char *states, int *lastclocks, zbx_uint64_t *lastlogsizes,
		int *mtimes, int *errcodes, size_t num)
{
	LOCK_CACHE;

	dc_requeue_items(itemids, states, lastclocks, lastlogsizes, mtimes, errcodes, num);

	UNLOCK_CACHE;
}

void	DCpoller_requeue_items(zbx_uint64_t *itemids, unsigned char *states, int *lastclocks, zbx_uint64_t *lastlogsizes,
		int *mtimes, int *errcodes, size_t num, unsigned char poller_type, int *nextcheck)
{
	LOCK_CACHE;

	dc_requeue_items(itemids, states, lastclocks, lastlogsizes, mtimes, errcodes, num);
	*nextcheck = dc_config_get_queue_nextcheck(&config->queues[poller_type]);

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DChost_get_agent_availability                                    *
 *                                                                            *
 * Purpose: get host availability data for the specified agent                *
 *                                                                            *
 * Parameters: dc_host      - [IN] the host                                   *
 *             agent        - [IN] the agent (see ZBX_FLAGS_AGENT_STATUS_*    *
 *                                 defines                                    *
 *             availability - [OUT] the host availability data                *
 *                                                                            *
 * Comments: The configuration cache must be locked already.                  *
 *                                                                            *
 ******************************************************************************/
static void	DChost_get_agent_availability(const ZBX_DC_HOST *dc_host, unsigned char agent_type,
		zbx_agent_availability_t *agent)
{

	agent->flags = ZBX_FLAGS_AGENT_STATUS;

	switch (agent_type)
	{
		case ZBX_AGENT_ZABBIX:
			agent->available = dc_host->available;
			agent->error = zbx_strdup(agent->error, dc_host->error);
			agent->errors_from = dc_host->errors_from;
			agent->disable_until = dc_host->disable_until;
			break;
		case ZBX_AGENT_SNMP:
			agent->available = dc_host->snmp_available;
			agent->error = zbx_strdup(agent->error, dc_host->snmp_error);
			agent->errors_from = dc_host->snmp_errors_from;
			agent->disable_until = dc_host->snmp_disable_until;
			break;
		case ZBX_AGENT_IPMI:
			agent->available = dc_host->ipmi_available;
			agent->error = zbx_strdup(agent->error, dc_host->ipmi_error);
			agent->errors_from = dc_host->ipmi_errors_from;
			agent->disable_until = dc_host->ipmi_disable_until;
			break;
		case ZBX_AGENT_JMX:
			agent->available = dc_host->jmx_available;
			agent->error = zbx_strdup(agent->error, dc_host->jmx_error);
			agent->errors_from = dc_host->jmx_errors_from;
			agent->disable_until = dc_host->jmx_disable_until;
			break;
	}
}

static void	DCagent_set_availability(zbx_agent_availability_t *av,  unsigned char *available, const char **error,
		int *errors_from, int *disable_until)
{
#define AGENT_AVAILABILITY_ASSIGN(flags, mask, dst, src)	\
	if (0 != (flags & mask))				\
	{							\
		if (dst != src)					\
			dst = src;				\
		else						\
			flags &= (~(mask));			\
	}

#define AGENT_AVAILABILITY_ASSIGN_STR(flags, mask, dst, src)	\
	if (0 != (flags & mask))				\
	{							\
		if (0 != strcmp(dst, src))			\
			DCstrpool_replace(1, &dst, src);	\
		else						\
			flags &= (~(mask));			\
	}

	AGENT_AVAILABILITY_ASSIGN(av->flags, ZBX_FLAGS_AGENT_STATUS_AVAILABLE, *available, av->available);
	AGENT_AVAILABILITY_ASSIGN_STR(av->flags, ZBX_FLAGS_AGENT_STATUS_ERROR, *error, av->error);
	AGENT_AVAILABILITY_ASSIGN(av->flags, ZBX_FLAGS_AGENT_STATUS_ERRORS_FROM, *errors_from, av->errors_from);
	AGENT_AVAILABILITY_ASSIGN(av->flags, ZBX_FLAGS_AGENT_STATUS_DISABLE_UNTIL, *disable_until, av->disable_until);

#undef AGENT_AVAILABILITY_ASSIGN_STR
#undef AGENT_AVAILABILITY_ASSIGN
}

/******************************************************************************
 *                                                                            *
 * Function: DChost_set_availability                                          *
 *                                                                            *
 * Purpose: set host availability data in configuration cache                 *
 *                                                                            *
 * Parameters: dc_host      - [OUT] the host                                  *
 *             availability - [IN/OUT] the host availability data             *
 *                                                                            *
 * Return value: SUCCEED - at least one availability field was updated        *
 *               FAIL    - no availability fields were updated                *
 *                                                                            *
 * Comments: The configuration cache must be locked already.                  *
 *                                                                            *
 *           This function clears availability flags of non updated fields    *
 *           updated leaving only flags identifying changed fields.           *
 *                                                                            *
 ******************************************************************************/
static int	DChost_set_agent_availability(ZBX_DC_HOST *dc_host, int now, unsigned char agent_type,
		zbx_agent_availability_t *agent)
{
	switch (agent_type)
	{
		case ZBX_AGENT_ZABBIX:
			DCagent_set_availability(agent, &dc_host->available,
					&dc_host->error, &dc_host->errors_from, &dc_host->disable_until);
			break;
		case ZBX_AGENT_SNMP:
			DCagent_set_availability(agent, &dc_host->snmp_available,
					&dc_host->snmp_error, &dc_host->snmp_errors_from, &dc_host->snmp_disable_until);
			break;
		case ZBX_AGENT_IPMI:
			DCagent_set_availability(agent, &dc_host->ipmi_available,
					&dc_host->ipmi_error, &dc_host->ipmi_errors_from, &dc_host->ipmi_disable_until);
			break;
		case ZBX_AGENT_JMX:
			DCagent_set_availability(agent, &dc_host->jmx_available,
					&dc_host->jmx_error, &dc_host->jmx_errors_from, &dc_host->jmx_disable_until);
			break;
	}

	if (ZBX_FLAGS_AGENT_STATUS_NONE == agent->flags)
		return FAIL;

	if (0 != (agent->flags & (ZBX_FLAGS_AGENT_STATUS_AVAILABLE | ZBX_FLAGS_AGENT_STATUS_ERROR)))
		dc_host->availability_ts = now;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: DChost_set_availability                                          *
 *                                                                            *
 * Purpose: set host availability data in configuration cache                 *
 *                                                                            *
 * Parameters: dc_host      - [OUT] the host                                  *
 *             availability - [IN/OUT] the host availability data             *
 *                                                                            *
 * Return value: SUCCEED - at least one availability field was updated        *
 *               FAIL    - no availability fields were updated                *
 *                                                                            *
 * Comments: The configuration cache must be locked already.                  *
 *                                                                            *
 *           This function clears availability flags of non updated fields    *
 *           updated leaving only flags identifying changed fields.           *
 *                                                                            *
 ******************************************************************************/
static int	DChost_set_availability(ZBX_DC_HOST *dc_host, int now, zbx_host_availability_t *ha)
{
	int		i;
	unsigned char	flags = ZBX_FLAGS_AGENT_STATUS_NONE;

	DCagent_set_availability(&ha->agents[ZBX_AGENT_ZABBIX], &dc_host->available, &dc_host->error,
			&dc_host->errors_from, &dc_host->disable_until);
	DCagent_set_availability(&ha->agents[ZBX_AGENT_SNMP], &dc_host->snmp_available, &dc_host->snmp_error,
			&dc_host->snmp_errors_from, &dc_host->snmp_disable_until);
	DCagent_set_availability(&ha->agents[ZBX_AGENT_IPMI], &dc_host->ipmi_available, &dc_host->ipmi_error,
			&dc_host->ipmi_errors_from, &dc_host->ipmi_disable_until);
	DCagent_set_availability(&ha->agents[ZBX_AGENT_JMX], &dc_host->jmx_available, &dc_host->jmx_error,
			&dc_host->jmx_errors_from, &dc_host->jmx_disable_until);

	for (i = 0; i < ZBX_AGENT_MAX; i++)
		flags |= ha->agents[i].flags;

	if (ZBX_FLAGS_AGENT_STATUS_NONE == flags)
		return FAIL;

	if (0 != (flags & (ZBX_FLAGS_AGENT_STATUS_AVAILABLE | ZBX_FLAGS_AGENT_STATUS_ERROR)))
		dc_host->availability_ts = now;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_host_availability_init                                       *
 *                                                                            *
 * Purpose: initializes host availability data                                *
 *                                                                            *
 * Parameters: availability - [IN/OUT] host availability data                 *
 *                                                                            *
 ******************************************************************************/
void	zbx_host_availability_init(zbx_host_availability_t *availability, zbx_uint64_t hostid)
{
	memset(availability, 0, sizeof(zbx_host_availability_t));
	availability->hostid = hostid;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_host_availability_clean                                      *
 *                                                                            *
 * Purpose: releases resources allocated to store host availability data      *
 *                                                                            *
 * Parameters: availability - [IN] host availability data                     *
 *                                                                            *
 ******************************************************************************/
void	zbx_host_availability_clean(zbx_host_availability_t *ha)
{
	int	i;

	for (i = 0; i < ZBX_AGENT_MAX; i++)
		zbx_free(ha->agents[i].error);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_host_availability_free                                       *
 *                                                                            *
 * Purpose: frees host availability data                                      *
 *                                                                            *
 * Parameters: availability - [IN] host availability data                     *
 *                                                                            *
 ******************************************************************************/
void	zbx_host_availability_free(zbx_host_availability_t *availability)
{
	zbx_host_availability_clean(availability);
	zbx_free(availability);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_agent_availability_init                                      *
 *                                                                            *
 * Purpose: initializes agent availability with the specified data            *
 *                                                                            *
 * Parameters: availability  - [IN/OUT] agent availability data               *
 *             hostid        - [IN] the host identifier                       *
 *             flags         - [IN] the availability flags indicating which   *
 *                                  availability fields to set                *
 *             available     - [IN] the availability data                     *
 *             error         - [IN]                                           *
 *             errors_from   - [IN]                                           *
 *             disable_until - [IN]                                           *
 *                                                                            *
 ******************************************************************************/
static void	zbx_agent_availability_init(zbx_agent_availability_t *agent, unsigned char available, const char *error,
		int errors_from, int disable_until)
{
	agent->flags = ZBX_FLAGS_AGENT_STATUS;
	agent->available = available;
	agent->error = zbx_strdup(agent->error, error);
	agent->errors_from = errors_from;
	agent->disable_until = disable_until;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_host_availability_init                                       *
 *                                                                            *
 * Purpose: checks host availability if any agent availability field is set   *
 *                                                                            *
 * Parameters: availability - [IN] host availability data                     *
 *                                                                            *
 * Return value: SUCCEED - an agent availability field is set                 *
 *               FAIL - no agent availability fields are set                  *
 *                                                                            *
 ******************************************************************************/
int	zbx_host_availability_is_set(const zbx_host_availability_t *ha)
{
	int	i;

	for (i = 0; i < ZBX_AGENT_MAX; i++)
	{
		if (ZBX_FLAGS_AGENT_STATUS_NONE != ha->agents[i].flags)
			return SUCCEED;
	}

	return FAIL;
}

/**************************************************************************************
 *                                                                                    *
 * Host availability update example                                                   *
 *                                                                                    *
 *                                                                                    *
 *               |            UnreachablePeriod                                       *
 *               |               (conf file)                                          *
 *               |              ______________                                        *
 *               |             /              \                                       *
 *               |             p     p     p     p       p       p                    *
 *               |             o     o     o     o       o       o                    *
 *               |             l     l     l     l       l       l                    *
 *               |             l     l     l     l       l       l                    *
 *               | n                                                                  *
 *               | e           e     e     e     e       e       e                    *
 *     agent     | w   p   p   r     r     r     r       r       r       p   p   p    *
 *       polls   |     o   o   r     r     r     r       r       r       o   o   o    *
 *               | h   l   l   o     o     o     o       o       o       l   l   l    *
 *               | o   l   l   r     r     r     r       r       r       l   l   l    *
 *               | s                                                                  *
 *               | t   ok  ok  E1    E1    E2    E1      E1      E2      ok  ok  ok   *
 *  --------------------------------------------------------------------------------  *
 *  available    | 0   1   1   1     1     1     2       2       2       0   0   0    *
 *               |                                                                    *
 *  error        | ""  ""  ""  ""    ""    ""    E1      E1      E2      ""  ""  ""   *
 *               |                                                                    *
 *  errors_from  | 0   0   0   T4    T4    T4    T4      T4      T4      0   0   0    *
 *               |                                                                    *
 *  disable_until| 0   0   0   T5    T6    T7    T8      T9      T10     0   0   0    *
 *  --------------------------------------------------------------------------------  *
 *   timestamps  | T1  T2  T3  T4    T5    T6    T7      T8      T9     T10 T11 T12   *
 *               |  \_/ \_/ \_/ \___/ \___/ \___/ \_____/ \_____/ \_____/ \_/ \_/     *
 *               |   |   |   |    |     |     |      |       |       |     |   |      *
 *  polling      |  item delay   UnreachableDelay    UnavailableDelay     item |      *
 *      periods  |                 (conf file)         (conf file)         delay      *
 *                                                                                    *
 *                                                                                    *
 **************************************************************************************/

/******************************************************************************
 *                                                                            *
 * Function: DChost_activate                                                  *
 *                                                                            *
 * Purpose: set host as available based on the agent availability data        *
 *                                                                            *
 * Parameters: hostid     - [IN] the host identifier                          *
 *             agent_type - [IN] the agent type (see ZBX_AGENT_* defines)     *
 *             ts         - [IN] the last timestamp                           *
 *             in         - [IN/OUT] IN: the caller's agent availability data *
 *                                  OUT: the agent availability data in cache *
 *                                       before changes                       *
 *             out        - [OUT] the agent availability data after changes   *
 *                                                                            *
 * Return value: SUCCEED - the host was activated successfully                *
 *               FAIL    - the host was already activated or activation       *
 *                         failed                                             *
 *                                                                            *
 * Comments: The host availability fields are updated according to the above  *
 *           schema.                                                          *
 *                                                                            *
 ******************************************************************************/
int	DChost_activate(zbx_uint64_t hostid, unsigned char agent_type, const zbx_timespec_t *ts,
		zbx_agent_availability_t *in, zbx_agent_availability_t *out)
{
	int		ret = FAIL;
	ZBX_DC_HOST	*dc_host;

	/* don't try activating host if there were no errors detected */
	if (0 == in->errors_from && HOST_AVAILABLE_TRUE == in->available)
		goto out;

	LOCK_CACHE;

	if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &hostid)))
		goto unlock;

	/* Don't try activating host if:                  */
	/* - (server, proxy) it's not monitored any more; */
	/* - (server) it's monitored by proxy.            */
	if ((0 != (program_type & ZBX_PROGRAM_TYPE_SERVER) && 0 != dc_host->proxy_hostid) ||
			HOST_STATUS_MONITORED != dc_host->status)
	{
		goto unlock;
	}

	DChost_get_agent_availability(dc_host, agent_type, in);
	zbx_agent_availability_init(out, HOST_AVAILABLE_TRUE, "", 0, 0);
	DChost_set_agent_availability(dc_host, ts->sec, agent_type, out);

	if (ZBX_FLAGS_AGENT_STATUS_NONE != out->flags)
		ret = SUCCEED;
unlock:
	UNLOCK_CACHE;
out:
	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DChost_deactivate                                                *
 *                                                                            *
 * Purpose: attempt to set host as unavailable based on agent availability    *
 *                                                                            *
 * Parameters: hostid     - [IN] the host identifier                          *
 *             agent_type - [IN] the agent type (see ZBX_AGENT_* defines)     *
 *             ts         - [IN] the last timestamp                           *
 *             in         - [IN/OUT] IN: the caller's host availability data  *
 *                                  OUT: the host availability data in cache  *
 *                                       before changes                       *
 *             out        - [OUT] the host availability data after changes    *
 *             error      - [IN] the error message                            *
 *                                                                            *
 * Return value: SUCCEED - the host was deactivated successfully              *
 *               FAIL    - the host was already deactivated or deactivation   *
 *                         failed                                             *
 *                                                                            *
 * Comments: The host availability fields are updated according to the above  *
 *           schema.                                                          *
 *                                                                            *
 ******************************************************************************/
int	DChost_deactivate(zbx_uint64_t hostid, unsigned char agent_type, const zbx_timespec_t *ts,
		zbx_agent_availability_t *in, zbx_agent_availability_t *out, const char *error_msg)
{
	int		ret = FAIL, errors_from,disable_until;
	const char	*error;
	unsigned char	available;
	ZBX_DC_HOST	*dc_host;


	/* don't try deactivating host if the unreachable delay has not passed since the first error */
	if (CONFIG_UNREACHABLE_DELAY > ts->sec - in->errors_from)
		goto out;

	LOCK_CACHE;

	if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &hostid)))
		goto unlock;

	/* Don't try deactivating host if:                */
	/* - (server, proxy) it's not monitored any more; */
	/* - (server) it's monitored by proxy.            */
	if ((0 != (program_type & ZBX_PROGRAM_TYPE_SERVER) && 0 != dc_host->proxy_hostid) ||
			HOST_STATUS_MONITORED != dc_host->status)
	{
		goto unlock;
	}

	DChost_get_agent_availability(dc_host, agent_type, in);

	available = in->available;
	error = in->error;

	if (0 == in->errors_from)
	{
		/* first error, schedule next unreachable check */
		errors_from = ts->sec;
		disable_until = ts->sec + CONFIG_UNREACHABLE_DELAY;
	}
	else
	{
		errors_from = in->errors_from;
		disable_until = in->disable_until;

		/* Check if other pollers haven't already attempted deactivating host. */
		/* In that case should wait the initial unreachable delay before       */
		/* trying to make it unavailable.                                      */
		if (CONFIG_UNREACHABLE_DELAY <= ts->sec - errors_from)
		{
			/* repeating error */
			if (CONFIG_UNREACHABLE_PERIOD > ts->sec - errors_from)
			{
				/* leave host available, schedule next unreachable check */
				disable_until = ts->sec + CONFIG_UNREACHABLE_DELAY;
			}
			else
			{
				/* make host unavailable, schedule next unavailable check */
				disable_until = ts->sec + CONFIG_UNAVAILABLE_DELAY;
				available = HOST_AVAILABLE_FALSE;
				error = error_msg;
			}
		}
	}

	zbx_agent_availability_init(out, available, error, errors_from, disable_until);
	DChost_set_agent_availability(dc_host, ts->sec, agent_type, out);

	if (ZBX_FLAGS_AGENT_STATUS_NONE != out->flags)
		ret = SUCCEED;
unlock:
	UNLOCK_CACHE;
out:
	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: DCset_hosts_availability                                         *
 *                                                                            *
 * Purpose: update availability of hosts in configuration cache and return    *
 *          the updated field flags                                           *
 *                                                                            *
 * Parameters: availabilities - [IN/OUT] the hosts availability data          *
 *                                                                            *
 * Return value: SUCCEED - at least one host availability data was updated    *
 *               FAIL    - no hosts were updated                              *
 *                                                                            *
 ******************************************************************************/
int	DCset_hosts_availability(zbx_vector_ptr_t *availabilities)
{
	int			i;
	ZBX_DC_HOST		*dc_host;
	zbx_host_availability_t	*ha;
	int			ret = FAIL, now;

	now = time(NULL);

	LOCK_CACHE;

	for (i = 0; i < availabilities->values_num; i++)
	{
		ha = (zbx_host_availability_t *)availabilities->values[i];

		if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &ha->hostid)) ||
				HOST_STATUS_MONITORED != dc_host->status)
		{
			int	j;

			/* reset availability flags so this host is ignored when saving availability diff to DB */
			for (j = 0; j < ZBX_AGENT_MAX; j++)
				ha->agents[j].flags = ZBX_FLAGS_AGENT_STATUS_NONE;

			continue;
		}

		if (SUCCEED == DChost_set_availability(dc_host, now, ha))
			ret = SUCCEED;
	}

	UNLOCK_CACHE;

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Comments: helper function for DCconfig_check_trigger_dependencies()        *
 *                                                                            *
 ******************************************************************************/
static int	DCconfig_check_trigger_dependencies_rec(const ZBX_DC_TRIGGER_DEPLIST *trigdep, int level)
{
	int				i;
	const ZBX_DC_TRIGGER		*next_trigger;
	const ZBX_DC_TRIGGER_DEPLIST	*next_trigdep;

	if (ZBX_TRIGGER_DEPENDENCY_LEVELS_MAX < level)
	{
		zabbix_log(LOG_LEVEL_CRIT, "recursive trigger dependency is too deep (triggerid:" ZBX_FS_UI64 ")",
				trigdep->triggerid);
		return SUCCEED;
	}

	if (0 != trigdep->dependencies.values_num)
	{
		for (i = 0; i < trigdep->dependencies.values_num; i++)
		{
			next_trigdep = (const ZBX_DC_TRIGGER_DEPLIST *)trigdep->dependencies.values[i];

			if (NULL != (next_trigger = next_trigdep->trigger) &&
					TRIGGER_VALUE_PROBLEM == next_trigger->value &&
					TRIGGER_STATUS_ENABLED == next_trigger->status &&
					TRIGGER_FUNCTIONAL_TRUE == next_trigger->functional)
			{
				return FAIL;
			}

			if (FAIL == DCconfig_check_trigger_dependencies_rec(next_trigdep, level + 1))
				return FAIL;
		}
	}

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_check_trigger_dependencies                              *
 *                                                                            *
 * Purpose: check whether any of trigger dependencies have value PROBLEM      *
 *                                                                            *
 * Return value: SUCCEED - trigger can change its value                       *
 *               FAIL - otherwise                                             *
 *                                                                            *
 * Author: Alexei Vladishev, Aleksandrs Saveljevs                             *
 *                                                                            *
 ******************************************************************************/
int	DCconfig_check_trigger_dependencies(zbx_uint64_t triggerid)
{
	int				ret = SUCCEED;
	const ZBX_DC_TRIGGER_DEPLIST	*trigdep;

	LOCK_CACHE;

	if (NULL != (trigdep = zbx_hashset_search(&config->trigdeps, &triggerid)))
		ret = DCconfig_check_trigger_dependencies_rec(trigdep, 0);

	UNLOCK_CACHE;

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Comments: helper function for DCconfig_sort_triggers_topologically()       *
 *                                                                            *
 ******************************************************************************/
static unsigned char	DCconfig_sort_triggers_topologically_rec(const ZBX_DC_TRIGGER_DEPLIST *trigdep, int level)
{
	int				i;
	unsigned char			topoindex = 2, next_topoindex;
	const ZBX_DC_TRIGGER_DEPLIST	*next_trigdep;

	if (32 < level)
	{
		zabbix_log(LOG_LEVEL_CRIT, "recursive trigger dependency is too deep (triggerid:" ZBX_FS_UI64 ")",
				trigdep->triggerid);
		goto exit;
	}

	if (0 == trigdep->trigger->topoindex)
	{
		zabbix_log(LOG_LEVEL_CRIT, "trigger dependencies contain a cycle (triggerid:" ZBX_FS_UI64 ")",
				trigdep->triggerid);
		goto exit;
	}

	trigdep->trigger->topoindex = 0;

	for (i = 0; i < trigdep->dependencies.values_num; i++)
	{
		next_trigdep = (const ZBX_DC_TRIGGER_DEPLIST *)trigdep->dependencies.values[i];

		if (1 < (next_topoindex =  next_trigdep->trigger->topoindex))
			goto next;

		if (0 == next_trigdep->dependencies.values_num)
			continue;

		next_topoindex = DCconfig_sort_triggers_topologically_rec(next_trigdep, level + 1);
next:
		if (topoindex < next_topoindex + 1)
			topoindex = next_topoindex + 1;
	}

	trigdep->trigger->topoindex = topoindex;
exit:
	return topoindex;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_sort_triggers_topologically                             *
 *                                                                            *
 * Purpose: assign each trigger an index based on trigger dependency topology *
 *                                                                            *
 * Author: Aleksandrs Saveljevs                                               *
 *                                                                            *
 ******************************************************************************/
static void	DCconfig_sort_triggers_topologically(void)
{
	zbx_hashset_iter_t		iter;
	ZBX_DC_TRIGGER			*trigger;
	const ZBX_DC_TRIGGER_DEPLIST	*trigdep;

	zbx_hashset_iter_reset(&config->trigdeps, &iter);

	while (NULL != (trigdep = zbx_hashset_iter_next(&iter)))
	{
		trigger = trigdep->trigger;

		if ((NULL != trigger && 1 < trigger->topoindex) || 0 == trigdep->dependencies.values_num)
			continue;

		DCconfig_sort_triggers_topologically_rec(trigdep, 0);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_set_trigger_value                                       *
 *                                                                            *
 * Purpose: set trigger value, value flags, and error                         *
 *                                                                            *
 * Author: Aleksandrs Saveljevs                                               *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_set_trigger_value(zbx_uint64_t triggerid, unsigned char value,
		unsigned char state, const char *error, int *lastchange)
{
	ZBX_DC_TRIGGER	*dc_trigger;

	LOCK_CACHE;

	if (NULL != (dc_trigger = zbx_hashset_search(&config->triggers, &triggerid)))
	{
		DCstrpool_replace(1, &dc_trigger->error, error);
		dc_trigger->value = value;
		dc_trigger->state = state;
		if (NULL != lastchange)
			dc_trigger->lastchange = *lastchange;
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_set_maintenance                                         *
 *                                                                            *
 * Purpose: set host maintenance status                                       *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_set_maintenance(const zbx_uint64_t *hostids, int hostids_num, int maintenance_status,
		int maintenance_type, int maintenance_from)
{
	int		i, now;
	ZBX_DC_HOST	*dc_host;

	now = time(NULL);

	LOCK_CACHE;

	for (i = 0; i < hostids_num; i++)
	{
		if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &hostids[i])))
			continue;

		if (HOST_STATUS_MONITORED != dc_host->status)
			continue;

		if (dc_host->maintenance_status != maintenance_status)
			dc_host->maintenance_from = maintenance_from;

		if (MAINTENANCE_TYPE_NODATA == dc_host->maintenance_type && MAINTENANCE_TYPE_NODATA != maintenance_type)
		{
			/* Store time at which no-data maintenance ended for the host (either */
			/* because no-data maintenance ended or because maintenance type was */
			/* changed to normal), this is needed for nodata() trigger function. */
			dc_host->data_expected_from = now;
		}

		dc_host->maintenance_status = maintenance_status;
		dc_host->maintenance_type = maintenance_type;
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_stats                                               *
 *                                                                            *
 * Purpose: get statistics of the database cache                              *
 *                                                                            *
 * Author: Alexander Vladishev, Aleksandrs Saveljevs                          *
 *                                                                            *
 ******************************************************************************/
void	*DCconfig_get_stats(int request)
{
	static zbx_uint64_t	value_uint;
	static double		value_double;

	const zbx_mem_info_t	*strpool_mem;

	strpool_mem = zbx_strpool_info()->mem_info;

	switch (request)
	{
		case ZBX_CONFSTATS_BUFFER_TOTAL:
			value_uint = config_mem->orig_size + strpool_mem->orig_size;
			return &value_uint;
		case ZBX_CONFSTATS_BUFFER_USED:
			value_uint = (config_mem->orig_size + strpool_mem->orig_size) -
					(config_mem->free_size + strpool_mem->free_size);
			return &value_uint;
		case ZBX_CONFSTATS_BUFFER_FREE:
			value_uint = config_mem->free_size + strpool_mem->free_size;
			return &value_uint;
		case ZBX_CONFSTATS_BUFFER_PFREE:
			value_double = 100.0 * ((double)(config_mem->free_size + strpool_mem->free_size) /
							(config_mem->orig_size + strpool_mem->orig_size));
			return &value_double;
		default:
			return NULL;
	}
}

static void	DCget_proxy(DC_PROXY *dst_proxy, ZBX_DC_PROXY *src_proxy)
{
	ZBX_DC_HOST		*host;
	ZBX_DC_INTERFACE_HT	*interface_ht, interface_ht_local;

	dst_proxy->hostid = src_proxy->hostid;
	dst_proxy->proxy_config_nextcheck = src_proxy->proxy_config_nextcheck;
	dst_proxy->proxy_data_nextcheck = src_proxy->proxy_data_nextcheck;

	if (NULL != (host = zbx_hashset_search(&config->hosts, &src_proxy->hostid)))
	{
		strscpy(dst_proxy->host, host->host);
		dst_proxy->tls_connect = host->tls_connect;

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
		if (ZBX_TCP_SEC_TLS_CERT == host->tls_connect)
		{
			strscpy(dst_proxy->tls_arg1, host->tls_issuer);
			strscpy(dst_proxy->tls_arg2, host->tls_subject);
		}
		else if (ZBX_TCP_SEC_TLS_PSK == host->tls_connect && NULL != host->tls_dc_psk)
		{
			strscpy(dst_proxy->tls_arg1, host->tls_dc_psk->tls_psk_identity);
			strscpy(dst_proxy->tls_arg2, host->tls_dc_psk->tls_psk);
		}
		else	/* ZBX_TCP_SEC_UNENCRYPTED */
#endif
		{
			*dst_proxy->tls_arg1 = '\0';
			*dst_proxy->tls_arg2 = '\0';
		}
	}
	else
	{
		/* DCget_proxy() is called only from DCconfig_get_proxypoller_hosts(), which is called only from */
		/* process_proxy(). So, this branch should never happen. */
		*dst_proxy->host = '\0';
		dst_proxy->tls_connect = ZBX_TCP_SEC_TLS_PSK;	/* set PSK to deliberately fail in this case */
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
		*dst_proxy->tls_arg1 = '\0';
		*dst_proxy->tls_arg2 = '\0';
#endif
		THIS_SHOULD_NEVER_HAPPEN;
	}

	interface_ht_local.hostid = src_proxy->hostid;
	interface_ht_local.type = INTERFACE_TYPE_UNKNOWN;

	if (NULL != (interface_ht = zbx_hashset_search(&config->interfaces_ht, &interface_ht_local)))
	{
		const ZBX_DC_INTERFACE	*interface = interface_ht->interface_ptr;

		strscpy(dst_proxy->addr_orig, interface->useip ? interface->ip : interface->dns);
		strscpy(dst_proxy->port_orig, interface->port);
	}
	else
	{
		*dst_proxy->addr_orig = '\0';
		*dst_proxy->port_orig = '\0';
	}

	dst_proxy->addr = NULL;
	dst_proxy->port = 0;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_proxypoller_hosts                                   *
 *                                                                            *
 * Purpose: Get array of proxies for proxy poller                             *
 *                                                                            *
 * Parameters: hosts - [OUT] array of hosts                                   *
 *             max_hosts - [IN] elements in hosts array                       *
 *                                                                            *
 * Return value: number of proxies in hosts array                             *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 * Comments: Proxies leave the queue only through this function. Pollers must *
 *           always return the proxies they have taken using DCrequeue_proxy. *
 *                                                                            *
 ******************************************************************************/
int	DCconfig_get_proxypoller_hosts(DC_PROXY *proxies, int max_hosts)
{
	const char		*__function_name = "DCconfig_get_proxypoller_hosts";
	int			now, num = 0;
	zbx_binary_heap_t	*queue;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	now = time(NULL);

	queue = &config->pqueue;

	LOCK_CACHE;

	while (num < max_hosts && FAIL == zbx_binary_heap_empty(queue))
	{
		const zbx_binary_heap_elem_t	*min;
		ZBX_DC_PROXY			*dc_proxy;

		min = zbx_binary_heap_find_min(queue);
		dc_proxy = (ZBX_DC_PROXY *)min->data;

		if (dc_proxy->proxy_config_nextcheck > now && dc_proxy->proxy_data_nextcheck > now)
			break;

		zbx_binary_heap_remove_min(queue);
		dc_proxy->location = ZBX_LOC_POLLER;

		DCget_proxy(&proxies[num], dc_proxy);
		num++;
	}

	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%d", __function_name, num);

	return num;
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_get_proxypoller_nextcheck                               *
 *                                                                            *
 * Purpose: Get nextcheck for passive proxies                                 *
 *                                                                            *
 * Return value: nextcheck or FAIL if no passive proxies in queue             *
 *                                                                            *
 * Author: Alexander Vladishev                                                *
 *                                                                            *
 ******************************************************************************/
int	DCconfig_get_proxypoller_nextcheck(void)
{
	const char		*__function_name = "DCconfig_get_proxypoller_nextcheck";

	int			nextcheck;
	zbx_binary_heap_t	*queue;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	queue = &config->pqueue;

	LOCK_CACHE;

	if (FAIL == zbx_binary_heap_empty(queue))
	{
		const zbx_binary_heap_elem_t	*min;
		const ZBX_DC_PROXY		*dc_proxy;

		min = zbx_binary_heap_find_min(queue);
		dc_proxy = (const ZBX_DC_PROXY *)min->data;

		nextcheck = (dc_proxy->proxy_config_nextcheck < dc_proxy->proxy_data_nextcheck) ?
				dc_proxy->proxy_config_nextcheck : dc_proxy->proxy_data_nextcheck;
	}
	else
		nextcheck = FAIL;

	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%d", __function_name, nextcheck);

	return nextcheck;
}

void	DCrequeue_proxy(zbx_uint64_t hostid, unsigned char update_nextcheck)
{
	const char	*__function_name = "DCrequeue_proxy";

	time_t		now;
	ZBX_DC_HOST	*dc_host;
	ZBX_DC_PROXY	*dc_proxy;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() update_nextcheck:%d", __function_name, (int)update_nextcheck);

	now = time(NULL);

	LOCK_CACHE;

	if (NULL != (dc_host = zbx_hashset_search(&config->hosts, &hostid)) &&
			NULL != (dc_proxy = zbx_hashset_search(&config->proxies, &hostid)))
	{
		if (ZBX_LOC_POLLER == dc_proxy->location)
			dc_proxy->location = ZBX_LOC_NOWHERE;

		if (HOST_STATUS_PROXY_PASSIVE == dc_host->status)
		{
			if (0 != (0x01 & update_nextcheck))
			{
				dc_proxy->proxy_config_nextcheck = (int)calculate_proxy_nextcheck(
						hostid, CONFIG_PROXYCONFIG_FREQUENCY, now);
			}
			if (0 != (0x02 & update_nextcheck))
			{
				dc_proxy->proxy_data_nextcheck = (int)calculate_proxy_nextcheck(
						hostid, CONFIG_PROXYDATA_FREQUENCY, now);
			}

			DCupdate_proxy_queue(dc_proxy);
		}
	}

	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCconfig_set_proxy_timediff                                      *
 *                                                                            *
 * Purpose: set rounded time difference between server clock and proxy clock  *
 *                                                                            *
 * Comments: When we calculate "nextcheck" for items on proxied hosts, we     *
 *           take the time difference between server and proxy into account.  *
 *                                                                            *
 *           For instance, suppose an item was processed on the proxy at 10   *
 *           seconds and there is a time difference between server and proxy  *
 *           of -2 seconds (that is, proxy's time is 2 seconds in front). In  *
 *           the server's database, we will store the timestamp of 8 seconds. *
 *           However, when we calculate "nextcheck" on the server side, we    *
 *           should calculate it from 10 seconds. Otherwise, if we calculate  *
 *           it from 8 seconds, we will probably get those 10 seconds as the  *
 *           "nextcheck". Finally, after calculating "nextcheck", we should   *
 *           utilize the time difference again to get Zabbix server's time.   *
 *                                                                            *
 *           Now, suppose we have a non-integer time difference, say, of -1.5 *
 *           seconds. Suppose also that one item on the proxy was checked at  *
 *           4.7 seconds, whereas a second item was checked at 5.2 seconds,   *
 *           which corresponds to server times of 3.2 and 3.7 seconds. Since  *
 *           "nextcheck" calculation only uses the integer part, server will  *
 *           use 3 seconds as the basis, before using the time difference.    *
 *           However, it is very likely that the first item was scheduled for *
 *           4 seconds on the proxy and the second item for 5 seconds, so in  *
 *           order to be precise we would need to store time differences for  *
 *           each individual item. That would lead to a significant increase  *
 *           in memory consumption, which we rather avoid. For this reason    *
 *           we only store a single time difference between server and proxy, *
 *           and use it for all items. However, the way time difference is    *
 *           used is different before and after "nextcheck" calculation.      *
 *                                                                            *
 *           Consider the example above. Server uses 3 seconds as the basis,  *
 *           and subtracts a unified difference of -2 (rounded down) to both  *
 *           items, yielding 5. It then calculates "nextcheck" for these two  *
 *           items, yielding 4 and 5 (assuming a one-minute delay). It then   *
 *           adds a unified difference of -1 (rounded up), to get Zabbix      *
 *           server's time of 3 and 4.                                        *
 *                                                                            *
 *           This has the effect that items will get into the delayed item    *
 *           queue at most one second later, but this is feasible: in proxy   *
 *           to server communication there is a communication latency, which  *
 *           also has the effect of putting items into the delayed item queue *
 *           a bit later, and we do not account for that anyway.              *
 *                                                                            *
 *           As described above, we subtract a rounded down difference before *
 *           "nextcheck" calculation and a rounded up difference after. Since *
 *           we have a 1/10^9 chance of hitting an integer "timediff" value,  *
 *           which would yield the same value rounded down and rounded up, in *
 *           the proxy structure we only store the difference rounded down to *
 *           save space. This is achieved by discarding the "ns" part of the  *
 *           "timediff" structure. We then assume that the rounded down value *
 *           and the rounded up values are different.                         *
 *                                                                            *
 *           Calculations of "nextchecks" themselves are done in functions    *
 *           DCget_reachable_nextcheck() and DCsync_items() functions.        *
 *                                                                            *
 ******************************************************************************/
void	DCconfig_set_proxy_timediff(zbx_uint64_t hostid, const zbx_timespec_t *timediff)
{
	ZBX_DC_PROXY	*dc_proxy;

	LOCK_CACHE;

	if (NULL != (dc_proxy = zbx_hashset_search(&config->proxies, &hostid)))
		dc_proxy->timediff = timediff->sec;

	UNLOCK_CACHE;
}

static void	dc_get_host_macro(zbx_uint64_t *hostids, int host_num, const char *macro, const char *context,
		char **value, char **value_default)
{
	int			i, j;
	ZBX_DC_HMACRO_HM	*hmacro_hm, hmacro_hm_local;
	ZBX_DC_HTMPL		*htmpl;
	zbx_vector_uint64_t	templateids;

	if (0 == host_num)
		return;

	hmacro_hm_local.macro = macro;

	for (i = 0; i < host_num; i++)
	{
		hmacro_hm_local.hostid = hostids[i];

		if (NULL != (hmacro_hm = zbx_hashset_search(&config->hmacros_hm, &hmacro_hm_local)))
		{
			for (j = 0; j < hmacro_hm->hmacros.values_num; j++)
			{
				ZBX_DC_HMACRO	*hmacro = (ZBX_DC_HMACRO *)hmacro_hm->hmacros.values[j];

				if (0 == strcmp(hmacro->macro, macro))
				{
					if (0 == zbx_strcmp_null(hmacro->context, context))
					{
						*value = zbx_strdup(*value, hmacro->value);
						return;
					}

					/* check for the default (without parameters) macro value */
					if (NULL == *value_default && NULL != context && NULL == hmacro->context)
						*value_default = zbx_strdup(*value_default, hmacro->value);
				}
			}
		}
	}

	zbx_vector_uint64_create(&templateids);
	zbx_vector_uint64_reserve(&templateids, 32);

	for (i = 0; i < host_num; i++)
	{
		if (NULL != (htmpl = zbx_hashset_search(&config->htmpls, &hostids[i])))
		{
			for (j = 0; j < htmpl->templateids.values_num; j++)
				zbx_vector_uint64_append(&templateids, htmpl->templateids.values[j]);
		}
	}

	if (0 != templateids.values_num)
	{
		zbx_vector_uint64_sort(&templateids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
		dc_get_host_macro(templateids.values, templateids.values_num, macro, context, value, value_default);
	}

	zbx_vector_uint64_destroy(&templateids);
}

static void	dc_get_global_macro(const char *macro, const char *context, char **value, char **value_default)
{
	int		i;
	ZBX_DC_GMACRO_M	*gmacro_m, gmacro_m_local;

	gmacro_m_local.macro = macro;

	if (NULL != (gmacro_m = zbx_hashset_search(&config->gmacros_m, &gmacro_m_local)))
	{
		for (i = 0; i < gmacro_m->gmacros.values_num; i++)
		{
			ZBX_DC_GMACRO	*gmacro = (ZBX_DC_GMACRO *)gmacro_m->gmacros.values[i];

			if (0 == strcmp(gmacro->macro, macro))
			{
				if (0 == zbx_strcmp_null(gmacro->context, context))
				{
					*value = zbx_strdup(*value, gmacro->value);
					break;
				}

				/* check for the default (without parameters) macro value */
				if (NULL == *value_default && NULL != context && NULL == gmacro->context)
					*value_default = zbx_strdup(*value_default, gmacro->value);
			}
		}
	}
}

static void	dc_get_user_macro(zbx_uint64_t *hostids, int hostids_num, const char *macro, const char *context,
		char **replace_to)
{
	char	*value = NULL, *value_default = NULL;

	/* User macros should be expanded according to the following priority: */
	/*                                                                     */
	/*  1) host context macro                                              */
	/*  2) global context macro                                            */
	/*  3) host base (default) macro                                       */
	/*  4) global base (default) macro                                     */
	/*                                                                     */
	/* We try to expand host macros first. If there is no perfect match on */
	/* the host level, we try to expand global macros, passing the default */
	/* macro value found on the host level, if any.                        */

	dc_get_host_macro(hostids, hostids_num, macro, context, &value, &value_default);

	if (NULL == value)
		dc_get_global_macro(macro, context, &value, &value_default);

	if (NULL != value)
	{
		zbx_free(*replace_to);
		*replace_to = value;

		zbx_free(value_default);
	}
	else if (NULL != value_default)
	{
		zbx_free(*replace_to);
		*replace_to = value_default;
	}
}

void	DCget_user_macro(zbx_uint64_t *hostids, int hostids_num, const char *macro, char **replace_to)
{
	const char	*__function_name = "DCget_user_macro";
	char		*name = NULL, *context = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() macro:'%s'", __function_name, macro);

	if (SUCCEED != zbx_user_macro_parse_dyn(macro, &name, &context, NULL))
		goto out;

	LOCK_CACHE;

	dc_get_user_macro(hostids, hostids_num, name, context, replace_to);

	UNLOCK_CACHE;

	zbx_free(context);
	zbx_free(name);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_expression_user_macro_validator                               *
 *                                                                            *
 * Purpose: validate user macro values in trigger expressions                 *
 *                                                                            *
 * Parameters: macro   - [IN] the user macro                                  *
 *             value   - [IN] the macro value                                 *
 *             error   - [OUT] the error message (optional)                   *
 *                                                                            *
 * Return value: SUCCEED - the macro value can be used in expression          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dc_expression_user_macro_validator(const char *macro, const char *value, char **error)
{
	if (SUCCEED == is_double_suffix(value, ZBX_FLAG_DOUBLE_SUFFIX))
		return SUCCEED;

	if (NULL != error)
		*error = zbx_dsprintf(*error, "macro '%s' value is not numeric", macro);

	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_expand_user_macros                                            *
 *                                                                            *
 * Purpose: expand user macros in the specified text value                    *
 *                                                                            *
 * Parameters: hostids        - [IN] an array of related hostids              *
 *             hostids_num    - [IN] the number of hostids                    *
 *             text           - [IN] the text value to expand                 *
 *             validator_func - [IN] an optional validator function           *
 *             error          - [OUT] the error message if expanding macros   *
 *                                    failed (optional)                       *
 *                                                                            *
 * Return value: The text value with expanded user macros.                    *
 *                                                                            *
 * Comments: The returned value must be freed by the caller.                  *
 *                                                                            *
 ******************************************************************************/
static char	*dc_expand_user_macros(const char *text, zbx_uint64_t *hostids, int hostids_num,
		zbx_value_validator_func_t validator_func, char **error)
{
	char		*exp = NULL, *macro = NULL, *name = NULL, *context = NULL, *value = NULL;
	const char	*ptr, *start;
	size_t		exp_alloc = 0, exp_offset = 0;
	int		len, ret = SUCCEED;

	for (start = text, ptr = strchr(start, '{'); NULL != ptr; ptr = strchr(ptr, '{'))
	{
		if ('$' != ptr[1] || SUCCEED != zbx_user_macro_parse_dyn(ptr, &name, &context, &len))
		{
			ptr++;
			continue;
		}

		zbx_strncpy_alloc(&exp, &exp_alloc, &exp_offset, start, ptr - start);

		dc_get_user_macro(hostids, hostids_num, name, context, &value);

		if (NULL != value)
		{
			if (NULL != validator_func)
			{
				macro = zbx_dsprintf(macro, "%.*s", len, ptr);
				ret = validator_func(macro, value, error);
			}

			if (SUCCEED == ret)
				zbx_strcpy_alloc(&exp, &exp_alloc, &exp_offset, value);

			zbx_free(value);
		}
		else
		{
			if (NULL != error)
				*error = zbx_dsprintf(*error, "macro '%.*s' is not found", len, ptr);

			ret = FAIL;
		}

		zbx_free(name);
		zbx_free(context);

		if (SUCCEED != ret)
		{
			zbx_free(exp);
			goto out;
		}

		start = ptr + len;
		ptr = start;
	}

	zbx_strcpy_alloc(&exp, &exp_alloc, &exp_offset, start);
out:
	zbx_free(macro);

	return exp;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_expression_expand_user_macros                                 *
 *                                                                            *
 * Purpose: expand user macros in trigger expression                          *
 *                                                                            *
 * Parameters: expression - [IN] the expression to expand                     *
 *             error      - [OUT] the error message                           *
 *                                                                            *
 * Return value: The expanded expression or NULL in the case of error.        *
 *               If NULL is returned the error message is set.                *
 *                                                                            *
 * Comments: The returned expression must be freed by the caller.             *
 *                                                                            *
 ******************************************************************************/
static char	*dc_expression_expand_user_macros(const char *expression, char **error)
{
	zbx_vector_uint64_t	functionids, hostids;
	char			*out;

	zbx_vector_uint64_create(&functionids);
	zbx_vector_uint64_create(&hostids);

	get_functionids(&functionids, expression);
	dc_get_hostids_by_functionids(&functionids, &hostids);

	out = dc_expand_user_macros(expression, hostids.values, hostids.values_num, dc_expression_user_macro_validator,
			error);

	zbx_vector_uint64_destroy(&hostids);
	zbx_vector_uint64_destroy(&functionids);

	return out;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_cache_expanded_expression                                     *
 *                                                                            *
 * Purpose: return expanded trigger expression, caching it if necessary       *
 *                                                                            *
 * Parameters: expression    - [IN] the expression to expand                  *
 *             expression_ex - [IN/OUT] the cached expression                 *
 *             error         - [OUT] the error message                        *
 *                                                                            *
 * Return value: The expanded expression, NULL in the case of error           *
 *                                                                            *
 * Comments: This function will first try to return a copy of cached          *
 *           expression. If the expression has not been expanded, it will     *
 *           expand the expression, cache it and return the copy of it.       *
 *                                                                            *
 ******************************************************************************/
static char	*dc_cache_expanded_expression(const char *expression, const char **expression_ex, char **error)
{
	char	*out;

	/* expression has already been cached, a copy */
	if (NULL != *expression_ex)
		return zbx_strdup(NULL, *expression_ex);

	/* expand expression and cache it if successful */
	if (NULL != (out = dc_expression_expand_user_macros(expression, error)))
		DCstrpool_replace(0, expression_ex, out);

	return out;
}

/******************************************************************************
 *                                                                            *
 * Function: DCexpression_expand_user_macros                                  *
 *                                                                            *
 * Purpose: expand user macros in trigger expression                          *
 *                                                                            *
 * Parameters: expression - [IN] the expression to expand                     *
 *             error      - [OUT] the error message                           *
 *                                                                            *
 * Return value: The expanded expression or NULL in the case of error.        *
 *               If NULL is returned the error message is set.                *
 *                                                                            *
 * Comments: The returned expression must be freed by the caller.             *
 *           This function is a locking wrapper of                            *
 *           dc_expression_expand_user_macros() function for external usage.  *
 *                                                                            *
 ******************************************************************************/
char	*DCexpression_expand_user_macros(const char *expression, char **error)
{
	char	*expression_ex;

	LOCK_CACHE;

	expression_ex = dc_expression_expand_user_macros(expression, error);

	UNLOCK_CACHE;

	return expression_ex;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_delta_items                                                *
 *                                                                            *
 * Purpose: Get a copy of delta item history stored in configuration cache    *
 *                                                                            *
 * Parameters: items - [OUT] the copy of item history                         *
 *             ids   - [IN] a vector of item ids to get the history for       *
 *                                                                            *
 * Comments: The hashset must be created by the caller like:                  *
 *            zbx_hashset_create(items, 1000, ZBX_DEFAULT_UINT64_HASH_FUNC,   *
 *                               ZBX_DEFAULT_UINT64_COMPARE_FUNC)             *
 *                                                                            *
 ******************************************************************************/
void	DCget_delta_items(zbx_hashset_t *items, const zbx_vector_uint64_t *ids)
{
	ZBX_DC_DELTAITEM	*deltaitem;
	int			i;

	LOCK_CACHE;

	/* only FLOAT and UINT64 value types can be used for delta calculations, */
	/* so just copying data is safe                                          */
	for (i = 0; i < ids->values_num; i++)
	{
		if (NULL != (deltaitem = zbx_hashset_search(&config->deltaitems, &ids->values[i])))
			zbx_hashset_insert(items, deltaitem, sizeof(ZBX_DC_DELTAITEM));
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCset_delta_items                                                *
 *                                                                            *
 * Purpose: Updates delta item history data in cache                          *
 *                                                                            *
 * Parameters: items - [IN] the new delta item history data. If the timestamp *
 *                          seconds is set to 0 then item history data is     *
 *                          removed from cache.                               *
 *                                                                            *
 ******************************************************************************/
void	DCset_delta_items(zbx_hashset_t *items)
{
	zbx_hashset_iter_t	iter;
	ZBX_DC_DELTAITEM	*deltaitem, *item;

	zbx_hashset_iter_reset(items, &iter);

	LOCK_CACHE;

	while (NULL != (item = zbx_hashset_iter_next(&iter)))
	{
		if (NULL == (deltaitem = zbx_hashset_search(&config->deltaitems, &item->itemid)))
		{
			if (0 != item->timestamp.sec)
				zbx_hashset_insert(&config->deltaitems, item, sizeof(ZBX_DC_DELTAITEM));
		}
		else
		{
			if (0 != item->timestamp.sec)
			{
				if (0 < zbx_timespec_compare(&item->timestamp, &deltaitem->timestamp))
				{
					deltaitem->timestamp = item->timestamp;
					deltaitem->value = item->value;
				}
			}
			else
				zbx_hashset_remove_direct(&config->deltaitems, deltaitem);
		}
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCfree_item_queue                                                *
 *                                                                            *
 * Purpose: frees the item queue data vector created by DCget_item_queue()    *
 *                                                                            *
 * Parameters: queue - [IN] the item queue data vector to free                *
 *                                                                            *
 ******************************************************************************/
void	DCfree_item_queue(zbx_vector_ptr_t *queue)
{
	int	i;

	for (i = 0; i < queue->values_num; i++)
		zbx_free(queue->values[i]);
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_item_queue                                                 *
 *                                                                            *
 * Purpose: retrieves vector of delayed items                                 *
 *                                                                            *
 * Parameters: queue - [OUT] the vector of delayed items (optional)           *
 *             from  - [IN] the minimum delay time in seconds (non-negative)  *
 *             to    - [IN] the maximum delay time in seconds or              *
 *                          ZBX_QUEUE_TO_INFINITY if there is no limit        *
 *                                                                            *
 * Return value: the number of delayed items                                  *
 *                                                                            *
 ******************************************************************************/
int	DCget_item_queue(zbx_vector_ptr_t *queue, int from, int to)
{
	zbx_hashset_iter_t	iter;
	const ZBX_DC_ITEM	*dc_item;
	int			now, nitems = 0, data_expected_from;
	zbx_queue_item_t	*queue_item;

	now = time(NULL);

	LOCK_CACHE;

	zbx_hashset_iter_reset(&config->items, &iter);

	while (NULL != (dc_item = zbx_hashset_iter_next(&iter)))
	{
		const ZBX_DC_HOST	*dc_host;

		if (ITEM_STATUS_ACTIVE != dc_item->status)
			continue;

		if (SUCCEED != is_counted_in_item_queue(dc_item->type, dc_item->key))
			continue;

		if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)))
			continue;

		if (HOST_STATUS_MONITORED != dc_host->status)
			continue;

		if (SUCCEED == DCin_maintenance_without_data_collection(dc_host, dc_item))
			continue;

		switch (dc_item->type)
		{
			case ITEM_TYPE_ZABBIX:
				if (HOST_AVAILABLE_TRUE != dc_host->available)
					continue;
				break;
			case ITEM_TYPE_ZABBIX_ACTIVE:
				if (dc_host->data_expected_from > (data_expected_from = dc_item->data_expected_from))
					data_expected_from = dc_host->data_expected_from;
				if (data_expected_from + dc_item->delay > now)
					continue;
				break;
			case ITEM_TYPE_SNMPv1:
			case ITEM_TYPE_SNMPv2c:
			case ITEM_TYPE_SNMPv3:
				if (HOST_AVAILABLE_TRUE != dc_host->snmp_available)
					continue;
				break;
			case ITEM_TYPE_IPMI:
				if (HOST_AVAILABLE_TRUE != dc_host->ipmi_available)
					continue;
				break;
			case ITEM_TYPE_JMX:
				if (HOST_AVAILABLE_TRUE != dc_host->jmx_available)
					continue;
				break;
		}

		if (now - dc_item->nextcheck < from || (ZBX_QUEUE_TO_INFINITY != to && now - dc_item->nextcheck >= to))
			continue;

		if (NULL != queue)
		{
			queue_item = zbx_malloc(NULL, sizeof(zbx_queue_item_t));
			queue_item->itemid = dc_item->itemid;
			queue_item->type = dc_item->type;
			queue_item->nextcheck = dc_item->nextcheck;
			queue_item->proxy_hostid = dc_host->proxy_hostid;

			zbx_vector_ptr_append(queue, queue_item);
		}
		nitems++;
	}

	UNLOCK_CACHE;

	return nitems;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_item_count                                                 *
 *                                                                            *
 * Purpose: return the number of active items                                 *
 *                                                                            *
 * Parameters: hostid - [IN] the host id, pass 0 to specify all hosts         *
 *                                                                            *
 * Return value: the number of active items                                   *
 *                                                                            *
 ******************************************************************************/
int	DCget_item_count(zbx_uint64_t hostid)
{
	int			count = 0;
	zbx_hashset_iter_t	iter;
	const ZBX_DC_ITEM	*dc_item;

	LOCK_CACHE;

	zbx_hashset_iter_reset(&config->items, &iter);

	while (NULL != (dc_item = zbx_hashset_iter_next(&iter)))
	{
		const ZBX_DC_HOST	*dc_host;

		if (ITEM_STATUS_ACTIVE != dc_item->status)
			continue;

		if (ZBX_FLAG_DISCOVERY_NORMAL != dc_item->flags && ZBX_FLAG_DISCOVERY_CREATED != dc_item->flags)
			continue;

		if (0 != hostid && hostid != dc_item->hostid)
			continue;

		if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)))
			continue;

		if (HOST_STATUS_MONITORED != dc_host->status)
			continue;

		count++;
	}

	UNLOCK_CACHE;

	return count;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_item_unsupported_count                                     *
 *                                                                            *
 * Purpose: return the number of active unsupported items                     *
 *                                                                            *
 * Parameters: hostid - [IN] the host id, pass 0 to specify all hosts         *
 *                                                                            *
 * Return value: the number of active unsupported items                       *
 *                                                                            *
 ******************************************************************************/
int	DCget_item_unsupported_count(zbx_uint64_t hostid)
{
	int			count = 0;
	zbx_hashset_iter_t	iter;
	const ZBX_DC_ITEM	*dc_item;

	LOCK_CACHE;

	zbx_hashset_iter_reset(&config->items, &iter);

	while (NULL != (dc_item = zbx_hashset_iter_next(&iter)))
	{
		const ZBX_DC_HOST	*dc_host;

		if (ITEM_STATUS_ACTIVE != dc_item->status || ITEM_STATE_NOTSUPPORTED != dc_item->state)
			continue;

		if (ZBX_FLAG_DISCOVERY_NORMAL != dc_item->flags && ZBX_FLAG_DISCOVERY_CREATED != dc_item->flags)
			continue;

		if (0 != hostid && hostid != dc_item->hostid)
			continue;

		if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)))
			continue;

		if (HOST_STATUS_MONITORED != dc_host->status)
			continue;

		count++;
	}

	UNLOCK_CACHE;

	return count;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_trigger_count                                              *
 *                                                                            *
 * Purpose: return the number of active triggers                              *
 *                                                                            *
 * Return value: the number of active triggers                                *
 *                                                                            *
 ******************************************************************************/
int	DCget_trigger_count(void)
{
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		functionid;
	const ZBX_DC_ITEM	*dc_item;
	const ZBX_DC_FUNCTION	*dc_function;
	const ZBX_DC_TRIGGER	*dc_trigger;
	const ZBX_DC_HOST	*dc_host;
	const char		*p, *q;
	int			count = 0;

	LOCK_CACHE;

	zbx_hashset_iter_reset(&config->triggers, &iter);

	while (NULL != (dc_trigger = zbx_hashset_iter_next(&iter)))
	{
		if (TRIGGER_STATUS_ENABLED != dc_trigger->status)
			continue;

		for (p = dc_trigger->expression; '\0' != *p; p++)
		{
			if ('{' != *p)
				continue;

			if ('$' == p[1])
			{
				int	macro_r, context_l, context_r;

				if (SUCCEED == zbx_user_macro_parse(p, &macro_r, &context_l, &context_r))
					p += macro_r;
				else
					p++;

				continue;
			}

			if (NULL == (q = strchr(p + 1, '}')))
				goto next;

			if (SUCCEED != is_uint64_n(p + 1, q - p - 1, &functionid))
					continue;

			if (NULL == (dc_function = zbx_hashset_search(&config->functions, &functionid)) ||
					NULL == (dc_item = zbx_hashset_search(&config->items, &dc_function->itemid)) ||
					ITEM_STATUS_ACTIVE != dc_item->status ||
					NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)) ||
					HOST_STATUS_MONITORED != dc_host->status)
			{
				goto next;
			}

			p = q;
		}

		count++;
next:;
	}

	UNLOCK_CACHE;

	return count;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_host_count                                                 *
 *                                                                            *
 * Purpose: return the number of monitored hosts                              *
 *                                                                            *
 * Return value: the number of monitored hosts                                *
 *                                                                            *
 ******************************************************************************/
int	DCget_host_count(void)
{
	int			nhosts = 0;
	zbx_hashset_iter_t	iter;
	const ZBX_DC_HOST	*dc_host;

	LOCK_CACHE;

	zbx_hashset_iter_reset(&config->hosts, &iter);

	while (NULL != (dc_host = zbx_hashset_iter_next(&iter)))
	{
		if (HOST_STATUS_MONITORED == dc_host->status)
			nhosts++;
	}

	UNLOCK_CACHE;

	return nhosts;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_required_performance                                       *
 *                                                                            *
 * Purpose: calculate the required server performance (values per second)     *
 *                                                                            *
 * Return value: the required nvps number                                     *
 *                                                                            *
 ******************************************************************************/
double	DCget_required_performance(void)
{
	double			nvps = 0;
	zbx_hashset_iter_t	iter;
	const ZBX_DC_ITEM	*dc_item;

	LOCK_CACHE;

	zbx_hashset_iter_reset(&config->items, &iter);

	while (NULL != (dc_item = zbx_hashset_iter_next(&iter)))
	{
		const ZBX_DC_HOST	*dc_host;

		if (ITEM_STATUS_ACTIVE != dc_item->status || 0 == dc_item->delay)
			continue;

		if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)))
			continue;

		if (HOST_STATUS_MONITORED != dc_host->status)
			continue;

		nvps += 1.0 / dc_item->delay;
	}

	UNLOCK_CACHE;

	return nvps;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_expressions_by_names                                       *
 *                                                                            *
 * Purpose: retrieves global expression data from cache                       *
 *                                                                            *
 * Parameters: expressions  - [OUT] a vector of expression data pointers      *
 *             names        - [IN] a vector containing expression names       *
 *             names_num    - [IN] the number of items in names vector        *
 *                                                                            *
 * Comment: The expressions vector contains allocated data, which must be     *
 *          freed afterwards with zbx_regexp_clean_expressions() function.    *
 *                                                                            *
 ******************************************************************************/
void	DCget_expressions_by_names(zbx_vector_ptr_t *expressions, const char * const *names, int names_num)
{
	int			i, iname;
	ZBX_DC_EXPRESSION	*expression;
	ZBX_DC_REGEXP		*regexp, search_regexp;

	LOCK_CACHE;

	for (iname = 0; iname < names_num; iname++)
	{
		search_regexp.name = names[iname];

		if (NULL != (regexp = zbx_hashset_search(&config->regexps, &search_regexp)))
		{
			for (i = 0; i < regexp->expressionids.values_num; i++)
			{
				zbx_uint64_t		expressionid = regexp->expressionids.values[i];
				zbx_expression_t	*rxp;

				if (NULL == (expression = zbx_hashset_search(&config->expressions, &expressionid)))
					continue;

				rxp = zbx_malloc(NULL, sizeof(zbx_expression_t));
				rxp->name = zbx_strdup(NULL, regexp->name);
				rxp->expression = zbx_strdup(NULL, expression->expression);
				rxp->exp_delimiter = expression->delimiter;
				rxp->case_sensitive = expression->case_sensitive;
				rxp->expression_type = expression->type;

				zbx_vector_ptr_append(expressions, rxp);
			}
		}
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_expression                                                 *
 *                                                                            *
 * Purpose: retrieves regular expression data from cache                      *
 *                                                                            *
 * Parameters: expressions  - [OUT] a vector of expression data pointers      *
 *             name         - [IN] the regular expression name                *
 *                                                                            *
 * Comment: The expressions vector contains allocated data, which must be     *
 *          freed afterwards with zbx_regexp_clean_expressions() function.    *
 *                                                                            *
 ******************************************************************************/
void	DCget_expressions_by_name(zbx_vector_ptr_t *expressions, const char *name)
{
	DCget_expressions_by_names(expressions, &name, 1);
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_data_expected_from                                         *
 *                                                                            *
 * Purpose: Returns time since which data is expected for the given item. We  *
 *          would not mind not having data for the item before that time, but *
 *          since that time we expect data to be coming.                      *
 *                                                                            *
 * Parameters: itemid  - [IN] the item id                                     *
 *             seconds - [OUT] the time data is expected as a Unix timestamp  *
 *                                                                            *
 ******************************************************************************/
int	DCget_data_expected_from(zbx_uint64_t itemid, int *seconds)
{
	ZBX_DC_ITEM	*dc_item;
	ZBX_DC_HOST	*dc_host;
	int		ret = FAIL;

	LOCK_CACHE;

	if (NULL == (dc_item = zbx_hashset_search(&config->items, &itemid)))
		goto unlock;

	if (ITEM_STATUS_ACTIVE != dc_item->status)
		goto unlock;

	if (NULL == (dc_host = zbx_hashset_search(&config->hosts, &dc_item->hostid)))
		goto unlock;

	if (HOST_STATUS_MONITORED != dc_host->status)
		goto unlock;

	*seconds = MAX(dc_item->data_expected_from, dc_host->data_expected_from);

	ret = SUCCEED;
unlock:
	UNLOCK_CACHE;

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_get_hostids_by_functionids                                    *
 *                                                                            *
 * Purpose: get function host ids grouped by an object (trigger) id           *
 *                                                                            *
 * Parameters: functionids - [IN] the function ids                            *
 *             hostids     - [OUT] the host ids                               *
 *                                                                            *
 ******************************************************************************/
static void	dc_get_hostids_by_functionids(zbx_vector_uint64_t *functionids, zbx_vector_uint64_t *hostids)
{
	ZBX_DC_FUNCTION	*function;
	ZBX_DC_ITEM	*item;
	int		i;

	for (i = 0; i < functionids->values_num; i++)
	{
		if (NULL == (function = zbx_hashset_search(&config->functions, &functionids->values[i])))
				continue;

		if (NULL != (item = zbx_hashset_search(&config->items, &function->itemid)))
			zbx_vector_uint64_append(hostids, item->hostid);
	}

	zbx_vector_uint64_sort(hostids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	zbx_vector_uint64_uniq(hostids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_hostids_by_functionids                                     *
 *                                                                            *
 * Purpose: get function host ids grouped by an object (trigger) id           *
 *                                                                            *
 * Parameters: functionids - [IN] the function ids                            *
 *             hostids     - [OUT] the host ids                               *
 *                                                                            *
 ******************************************************************************/
void	DCget_hostids_by_functionids(zbx_vector_uint64_t *functionids, zbx_vector_uint64_t *hostids)
{
	const char	*__function_name = "DCget_hostids_by_functionids";

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	LOCK_CACHE;

	dc_get_hostids_by_functionids(functionids, hostids);

	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(): found %d hosts", __function_name, hostids->values_num);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_config_get                                                   *
 *                                                                            *
 * Purpose: get global configuration data                                     *
 *                                                                            *
 * Parameters: cfg   - [OUT] the global configuration data                    *
 *             flags - [IN] the flags specifying fields to set,               *
 *                          see ZBX_CONFIG_FLAGS_ defines                     *
 *                                                                            *
 * Comments: It's recommended to cleanup this structure with zbx_config_clean *
 *           function even if only simple fields are requested.               *
 *                                                                            *
 ******************************************************************************/
void	zbx_config_get(zbx_config_t *cfg, zbx_uint64_t flags)
{
	LOCK_CACHE;

	if (0 != (flags & ZBX_CONFIG_FLAGS_SEVERITY_NAME))
	{
		int	i;

		cfg->severity_name = zbx_malloc(NULL, TRIGGER_SEVERITY_COUNT * sizeof(char *));

		for (i = 0; i < TRIGGER_SEVERITY_COUNT; i++)
			cfg->severity_name[i] = zbx_strdup(NULL, config->config->severity_name[i]);
	}

	if (0 != (flags & ZBX_CONFIG_FLAGS_DISCOVERY_GROUPID))
		cfg->discovery_groupid = config->config->discovery_groupid;

	if (0 != (flags & ZBX_CONFIG_FLAGS_DEFAULT_INVENTORY_MODE))
		cfg->default_inventory_mode = config->config->default_inventory_mode;

	if (0 != (flags & ZBX_CONFIG_FLAGS_REFRESH_UNSUPPORTED))
		cfg->refresh_unsupported = config->config->refresh_unsupported;

	if (0 != (flags & ZBX_CONFIG_FLAGS_SNMPTRAP_LOGGING))
		cfg->snmptrap_logging = config->config->snmptrap_logging;

	if (0 != (flags & ZBX_CONFIG_FLAGS_HOUSEKEEPER))
		cfg->hk = config->config->hk;

	UNLOCK_CACHE;

	cfg->flags = flags;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_config_clean                                                 *
 *                                                                            *
 * Purpose: cleans global configuration data structure filled                 *
 *          by zbx_config_get() function                                      *
 *                                                                            *
 * Parameters: cfg   - [IN] the global configuration data                     *
 *                                                                            *
 ******************************************************************************/
void	zbx_config_clean(zbx_config_t *cfg)
{
	if (0 != (cfg->flags & ZBX_CONFIG_FLAGS_SEVERITY_NAME))
	{
		int	i;

		for (i = 0; i < TRIGGER_SEVERITY_COUNT; i++)
			zbx_free(cfg->severity_name[i]);

		zbx_free(cfg->severity_name);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: DCreset_hosts_availability                                       *
 *                                                                            *
 * Purpose: resets host availability for disabled hosts and hosts without     *
 *          enabled items for the corresponding interface                     *
 *                                                                            *
 * Parameters: hosts - [OUT] changed host availability data                   *
 *                                                                            *
 * Return value: SUCCEED - host availability was reset for at least one host  *
 *               FAIL    - no hosts required availability reset               *
 *                                                                            *
 * Comments: This function resets host availability in configuration cache.   *
 *           The caller must perform corresponding database updates based     *
 *           on returned host availability reset data. On server the function *
 *           skips hosts handled by proxies.                                  *
 *                                                                            *
 ******************************************************************************/
int	DCreset_hosts_availability(zbx_vector_ptr_t *hosts)
{
	const char		*__function_name = "DCreset_hosts_availability";
	ZBX_DC_HOST		*host;
	zbx_hashset_iter_t	iter;
	zbx_host_availability_t	*ha = NULL;
	int			now;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	now = time(NULL);

	LOCK_CACHE;

	zbx_hashset_iter_reset(&config->hosts, &iter);

	while (NULL != (host = (ZBX_DC_HOST *)zbx_hashset_iter_next(&iter)))
	{
		/* On server skip hosts handled by proxies. They are handled directly */
		/* when receiving hosts' availability data from proxies.              */
		/* Unless a host was just (re)assigned to a proxy or the proxy has    */
		/* not updated its status during the maximum proxy heartbeat period.  */
		/* In this case reset all interfaces to unknown status.               */
		if (0 != (program_type & ZBX_PROGRAM_TYPE_SERVER) && 0 != host->proxy_hostid)
		{
			if (ZBX_FLAG_INTERFACE_UNKNOWN != host->used_interfaces)
			{
				ZBX_DC_PROXY	*proxy;

				if (NULL != (proxy = zbx_hashset_search(&config->proxies, &host->proxy_hostid)))
				{
					/* SEC_PER_MIN is a tolerance interval, it was chosen arbitrarily */
					if (ZBX_PROXY_HEARTBEAT_FREQUENCY_MAX + SEC_PER_MIN >= now - proxy->lastaccess)
						continue;
				}
				host->used_interfaces = ZBX_FLAG_INTERFACE_UNKNOWN;
			}
		}

		if (NULL == ha)
			ha = (zbx_host_availability_t *)zbx_malloc(NULL, sizeof(zbx_host_availability_t));

		zbx_host_availability_init(ha, host->hostid);

		if (0 == (host->used_interfaces & ZBX_FLAG_INTERFACE_ZABBIX) &&
				HOST_AVAILABLE_UNKNOWN != host->available)
		{
			zbx_agent_availability_init(&ha->agents[ZBX_AGENT_ZABBIX], HOST_AVAILABLE_UNKNOWN, "", 0, 0);
		}

		if (0 == (host->used_interfaces & ZBX_FLAG_INTERFACE_SNMP) &&
				HOST_AVAILABLE_UNKNOWN != host->snmp_available)
		{
			zbx_agent_availability_init(&ha->agents[ZBX_AGENT_SNMP], HOST_AVAILABLE_UNKNOWN, "", 0, 0);
		}

		if (0 == (host->used_interfaces & ZBX_FLAG_INTERFACE_IPMI) &&
				HOST_AVAILABLE_UNKNOWN != host->ipmi_available)
		{
			zbx_agent_availability_init(&ha->agents[ZBX_AGENT_IPMI], HOST_AVAILABLE_UNKNOWN, "", 0, 0);
		}

		if (0 == (host->used_interfaces & ZBX_FLAG_INTERFACE_JMX) &&
				HOST_AVAILABLE_UNKNOWN != host->jmx_available)
		{
			zbx_agent_availability_init(&ha->agents[ZBX_AGENT_JMX], HOST_AVAILABLE_UNKNOWN, "", 0, 0);
		}

		if (SUCCEED == zbx_host_availability_is_set(ha))
		{
			if (SUCCEED == DChost_set_availability(host, now, ha))
			{
				zbx_vector_ptr_append(hosts, ha);
				ha = NULL;
			}
			else
				zbx_host_availability_clean(ha);
		}
	}
	UNLOCK_CACHE;

	zbx_free(ha);

	zbx_vector_ptr_sort(hosts, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() hosts:%d", __function_name, hosts->values_num);

	return 0 == hosts->values_num ? FAIL : SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: DCget_hosts_availability                                         *
 *                                                                            *
 * Purpose: gets availability data for hosts with availability data changed   *
 *          in period from last availability update to the specified          *
 *          timestamp                                                         *
 *                                                                            *
 * Parameters: hosts - [OUT] changed host availability data                   *
 *             ts    - [OUT] the availability diff timestamp                  *
 *                                                                            *
 * Return value: SUCCEED - availability was changed for at least one host     *
 *               FAIL    - no host availability was changed                   *
 *                                                                            *
 ******************************************************************************/
int	DCget_hosts_availability(zbx_vector_ptr_t *hosts, int *ts)
{
	const char		*__function_name = "DCget_hosts_availability";
	ZBX_DC_HOST		*host;
	zbx_hashset_iter_t	iter;
	zbx_host_availability_t	*ha = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	LOCK_CACHE;

	*ts = time(NULL);

	zbx_hashset_iter_reset(&config->hosts, &iter);

	while (NULL != (host = (ZBX_DC_HOST *)zbx_hashset_iter_next(&iter)))
	{
		if (config->availability_diff_ts <= host->availability_ts && host->availability_ts < *ts)
		{
			ha = (zbx_host_availability_t *)zbx_malloc(NULL, sizeof(zbx_host_availability_t));
			zbx_host_availability_init(ha, host->hostid);

			zbx_agent_availability_init(&ha->agents[ZBX_AGENT_ZABBIX], host->available, host->error,
					host->errors_from, host->disable_until);
			zbx_agent_availability_init(&ha->agents[ZBX_AGENT_SNMP], host->snmp_available, host->snmp_error,
					host->snmp_errors_from, host->snmp_disable_until);
			zbx_agent_availability_init(&ha->agents[ZBX_AGENT_IPMI], host->ipmi_available, host->ipmi_error,
					host->ipmi_errors_from, host->ipmi_disable_until);
			zbx_agent_availability_init(&ha->agents[ZBX_AGENT_JMX], host->jmx_available, host->jmx_error,
					host->jmx_errors_from, host->jmx_disable_until);

			zbx_vector_ptr_append(hosts, ha);
		}
	}

	UNLOCK_CACHE;

	zbx_vector_ptr_sort(hosts, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() hosts:%d", __function_name, hosts->values_num);

	return 0 == hosts->values_num ? FAIL : SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_db_condition_free                                            *
 *                                                                            *
 * Purpose: frees condition data structure                                    *
 *                                                                            *
 * Parameters: condition - [IN] the condition data to free                    *
 *                                                                            *
 ******************************************************************************/
static void	zbx_db_condition_free(DB_CONDITION *condition)
{
	zbx_free(condition->value);
	zbx_free(condition);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_action_eval_free                                             *
 *                                                                            *
 * Purpose: frees action evaluation data structure                            *
 *                                                                            *
 * Parameters: action - [IN] the action evaluation to free                    *
 *                                                                            *
 ******************************************************************************/
void	zbx_action_eval_free(zbx_action_eval_t *action)
{
	zbx_free(action->formula);

	zbx_vector_ptr_clear_ext(&action->conditions, (zbx_clean_func_t)zbx_db_condition_free);
	zbx_vector_ptr_destroy(&action->conditions);

	zbx_free(action);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_action_copy_conditions                                        *
 *                                                                            *
 * Purpose: copies configuration cache action conditions to the specified     *
 *          vector                                                            *
 *                                                                            *
 * Parameters: dc_action  - [IN] the source action                            *
 *             conditions - [OUT] the conditions vector                       *
 *                                                                            *
 ******************************************************************************/
static void	dc_action_copy_conditions(const zbx_dc_action_t *dc_action, zbx_vector_ptr_t *conditions)
{
	int				i;
	DB_CONDITION			*condition;
	zbx_dc_action_condition_t	*dc_condition;

	zbx_vector_ptr_reserve(conditions, dc_action->conditions.values_num);

	for (i = 0; i < dc_action->conditions.values_num; i++)
	{
		dc_condition = (zbx_dc_action_condition_t *)dc_action->conditions.values[i];

		condition = (DB_CONDITION *)zbx_malloc(NULL, sizeof(DB_CONDITION));

		condition->conditionid = dc_condition->conditionid;
		condition->actionid = dc_action->actionid;
		condition->conditiontype = dc_condition->conditiontype;
		condition->operator = dc_condition->operator;
		condition->value = zbx_strdup(NULL, dc_condition->value);

		zbx_vector_ptr_append(conditions, condition);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: dc_action_eval_create                                            *
 *                                                                            *
 * Purpose: creates action evaluation data from configuration cache action    *
 *                                                                            *
 * Parameters: dc_action - [IN] the source action                             *
 *                                                                            *
 * Return value: the action evaluation data                                   *
 *                                                                            *
 * Comments: The returned value must be freed with zbx_action_eval_free()     *
 *           function later.                                                  *
 *                                                                            *
 ******************************************************************************/
static zbx_action_eval_t	*dc_action_eval_create(const zbx_dc_action_t *dc_action)
{
	zbx_action_eval_t		*action;

	action = (zbx_action_eval_t *)zbx_malloc(NULL, sizeof(zbx_action_eval_t));

	action->actionid = dc_action->actionid;
	action->eventsource = dc_action->eventsource;
	action->evaltype = dc_action->evaltype;
	action->formula = zbx_strdup(NULL, dc_action->formula);
	zbx_vector_ptr_create(&action->conditions);

	dc_action_copy_conditions(dc_action, &action->conditions);

	return action;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_get_actions_eval                                          *
 *                                                                            *
 * Purpose: gets action evaluation data                                       *
 *                                                                            *
 * Parameters: actions     - [OUT] the action evaluation data                 *
 *                                                                            *
 * Comments: The returned actions must be freed with zbx_action_eval_free()   *
 *           function later.                                                  *
 *                                                                            *
 ******************************************************************************/
void	zbx_dc_get_actions_eval(zbx_vector_ptr_t *actions)
{
	const char			*__function_name = "zbx_dc_get_actions_eval";
	zbx_dc_action_t			*dc_action;
	zbx_hashset_iter_t		iter;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	LOCK_CACHE;

	zbx_hashset_iter_reset(&config->actions, &iter);

	while (NULL != (dc_action = (zbx_dc_action_t *)zbx_hashset_iter_next(&iter)))
	{
		zbx_vector_ptr_append(actions, dc_action_eval_create(dc_action));
	}

	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() actions:%d", __function_name, actions->values_num);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_set_availability_update_ts                                   *
 *                                                                            *
 * Purpose: sets timestamp of the last availability update                    *
 *                                                                            *
 * Parameter: ts - [IN] the last availability update timestamp                *
 *                                                                            *
 * Comments: This function is used only by proxies when preparing host        *
 *           availability data to be sent to server.                          *
 *                                                                            *
 ******************************************************************************/
void	zbx_set_availability_diff_ts(int ts)
{
	/* this data can't be accessed simultaneously from multiple processes - locking is not necessary */
	config->availability_diff_ts = ts;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_update_proxy_lastaccess                                   *
 *                                                                            *
 * Purpose: updates proxy last access timestamp in configuration cache        *
 *                                                                            *
 * Parameter: hostid     - [IN] the proxy identifier (hostid)                 *
 *            lastaccess - [IN] the last time proxy data was received/sent    *
 *                                                                            *
 ******************************************************************************/
void zbx_dc_update_proxy_lastaccess(zbx_uint64_t hostid, int lastaccess)
{
	ZBX_DC_PROXY	*proxy;

	LOCK_CACHE;

	if (NULL != (proxy = (ZBX_DC_PROXY *)zbx_hashset_search(&config->proxies, &hostid)))
		proxy->lastaccess = lastaccess;

	UNLOCK_CACHE;
}
