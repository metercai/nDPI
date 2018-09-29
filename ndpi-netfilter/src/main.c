/* 
 * main.c
 * Copyright (C) 2010-2012 G. Elian Gidoni <geg@gnu.org>
 *               2012 Ed Wildgoose <lists@wildgooses.com>
 * 
 * This file is part of nDPI, an open source deep packet inspection
 * library based on the PACE technology by ipoque GmbH
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the License.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/netfilter/x_tables.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/if_ether.h>
#include <linux/rbtree.h>
#include <linux/kref.h>
#include <linux/time.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <linux/atomic.h>
#include <linux/proc_fs.h>

#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_extend.h>

#define BT_ANNOUNCE 

#include "ndpi_config.h"
#undef HAVE_HYPERSCAN
#include "ndpi_main.h"

#include "xt_ndpi.h"

#include "../lib/third_party/include/ndpi_patricia.h"
#include "../lib/third_party/include/ahocorasick.h"

extern ndpi_protocol_match host_match[];
/* Only for debug! */

//#define NDPI_IPPORT_DEBUG
#ifdef NDPI_IPPORT_DEBUG
#undef DP
#define DP(fmt, args...) printk(fmt, __func__, ## args)
#define DBGDATA(a...) a;
#warning  "DEBUG code"
#else
#define DP(fmt, args...)
#define DBGDATA(a...)
#endif

#define COUNTER(a) (volatile unsigned long int)(a)++

#define NDPI_PROCESS_ERROR (NDPI_NUM_BITS+1)
#ifndef IPPROTO_OSPF
#define IPPROTO_OSPF    89
#endif

static char dir_name[]="xt_ndpi";
static char info_name[]="info";
static char ipdef_name[]="ip_proto";
static char hostdef_name[]="host_proto";
#ifdef NDPI_DETECTION_SUPPORT_IPV6
static char info6_name[]="info6";
#endif
#ifdef BT_ANNOUNCE
static char ann_name[]="announce";
#endif

static char proto_name[]="proto";


#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)


#define PROC_REMOVE(pde,net) proc_remove(pde)
#else

#define PROC_REMOVE(pde,net) proc_net_remove(net,dir_name)

/* backport from 3.10 */
static inline struct inode *file_inode(struct file *f)
{
	return f->f_path.dentry->d_inode;
}
static inline void *PDE_DATA(const struct inode *inode)
{
	return PROC_I(inode)->pde->data;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)
#define refcount_dec_and_test(a) atomic_sub_and_test((int) 1,(a))
#endif

// for testing only!
//#define USE_CONNLABELS

#if !defined(USE_CONNLABELS) && defined(CONFIG_NF_CONNTRACK_CUSTOM) && CONFIG_NF_CONNTRACK_CUSTOM > 0
#define NF_CT_CUSTOM
#else
#undef NF_CT_CUSTOM
#include <net/netfilter/nf_conntrack_labels.h>
#ifndef CONFIG_NF_CONNTRACK_LABELS
#error NF_CONNTRACK_LABELS not defined
#endif
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("G. Elian Gidoni <geg@gnu.org>, Vitaly E. Lavrov <vel21ripn@gmail.com>");
MODULE_DESCRIPTION("nDPI wrapper");
MODULE_ALIAS("ipt_ndpi");
MODULE_ALIAS("ipt_NDPI");


/* simple strings collestion */
typedef struct str_collect {
	size_t	 max,last;
	char	 s[0];
} str_collect_t;

typedef struct hosts_str {
	str_collect_t *p[NDPI_NUM_BITS+1];
} hosts_str_t;

/* id tracking */
struct osdpi_id_node {
        struct rb_node node;
        struct kref refcnt;
	union nf_inet_addr ip;
	struct ndpi_id_struct ndpi_id;
};

typedef struct ndpi_detection_module_struct ndpi_mod_str_t;

typedef enum write_buf_id {
	W_BUF_IP=0,
	W_BUF_HOST,
	W_BUF_PROTO,
	W_BUF_LAST
} write_buf_id_t;

struct write_proc_cmd {
	uint32_t  cpos,max;
	char      cmd[0];
};

struct ndpi_net {
	struct ndpi_detection_module_struct *ndpi_struct;
	struct rb_root osdpi_id_root;
	NDPI_PROTOCOL_BITMASK protocols_bitmask;
	atomic_t	protocols_cnt[NDPI_NUM_BITS+1];
	spinlock_t	id_lock;
	spinlock_t	ipq_lock; // for proto & patricia tree
	struct proc_dir_entry   *pde,
#ifdef NDPI_DETECTION_SUPPORT_IPV6
				*pe_info6,
#endif
#ifdef BT_ANNOUNCE
				*pe_ann,
#endif
				*pe_info,
				*pe_proto,
				*pe_hostdef,
				*pe_ipdef;
	int		n_hash;
	int		gc_count;
	int		gc_index;
	int		labels_word;
        struct		timer_list gc;

	spinlock_t	host_lock; /* protect host_ac, hosts, hosts_tmp */
	hosts_str_t	*hosts;
	
	hosts_str_t	*hosts_tmp;
	void		*host_ac;
	int		host_error;

	spinlock_t	       w_buff_lock;
	struct write_proc_cmd *w_buff[W_BUF_LAST];

	struct ndpi_mark {
		uint32_t	mark,mask;
	} mark[NDPI_NUM_BITS+1];
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
	u_int8_t debug_level[NDPI_NUM_BITS+1];
#endif
};

struct nf_ct_ext_ndpi {
	struct ndpi_flow_struct	*flow;
	struct ndpi_id_struct   *src,*dst;
	char			*host;
	char			*ssl;
	ndpi_protocol		proto; // 2 x 2 bytes
	spinlock_t		lock; // 2 bytes on 32bit, 4 bytes on 64bit
	uint8_t			l4_proto, detect_done;
#if __SIZEOF_LONG__ != 4
	uint8_t			pad[6];
#endif
/* 
 * 32bit - 28 bytes, 64bit 56 bytes;
 */
} __attribute ((packed));

#ifndef NF_CT_CUSTOM

#define MAGIC_CT 0xa55a
struct nf_ct_ext_labels { /* max size 128 bit */
	/* words must be first byte for compatible with NF_CONNLABELS
	 * kernels 3.8-4.7 has variable size of nf_ext_labels
	 * kernels 4.8 has fixed size of nf_ext_labels
	 * 32bit - 8 bytes, 64bit - 16 bytes
	 */
	uint8_t			words,pad1;
	uint16_t		magic;
#if __SIZEOF_LONG__ != 4
	uint8_t			pad2[4];
#endif
	struct nf_ct_ext_ndpi	*ndpi_ext;
} __attribute ((packed));
#endif

struct ndpi_cb {
	void		*last_ct;
	uint32_t	data[2];
} __attribute ((packed));

struct ndpi_port_range {
	uint16_t	start, end, // port range
			proto,	    // ndpi proto
			l4_proto;   // 0 - udp, 1 - tcp
};
typedef struct ndpi_port_range ndpi_port_range_t;

struct ndpi_port_def {
	int		  count[2]; // counter udp and tcp ranges
	ndpi_port_range_t p[0];     // udp and tcp ranges
};

static ndpi_protocol proto_null = NDPI_PROTOCOL_NULL;

static unsigned long  ndpi_log_debug=0;
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
static unsigned long  ndpi_lib_trace=0;
#endif
static unsigned long  ndpi_mtu=48000;
static unsigned long  bt_log_size=128;
static unsigned long  bt_hash_size=0;
static unsigned long  bt_hash_tmo=1200;

static unsigned long  max_packet_unk_tcp=20;
static unsigned long  max_packet_unk_udp=20;
static unsigned long  max_packet_unk_other=20;

static unsigned long  ndpi_size_flow_struct=0;
static unsigned long  ndpi_size_id_struct=0;
static unsigned long  ndpi_size_hash_ip4p_node=0;

static unsigned long  ndpi_jumbo=0;
static unsigned long  ndpi_falloc=0;
static unsigned long  ndpi_nskb=0;
static unsigned long  ndpi_lskb=0;
static unsigned long  ndpi_flow_c=0;
static unsigned long  ndpi_flow_d=0;
static unsigned long  ndpi_bt_gc=0;

static unsigned long  ndpi_p0=0;
static unsigned long  ndpi_p1=0;
static unsigned long  ndpi_p2=0;
static unsigned long  ndpi_p31=0;
static unsigned long  ndpi_p34=0;
static unsigned long  ndpi_p7=0;
static unsigned long  ndpi_p9=0;
static unsigned long  ndpi_pa=0;
static unsigned long  ndpi_pb=0;
static unsigned long  ndpi_pc=0;
static unsigned long  ndpi_pd=0;
static unsigned long  ndpi_pe=0;
static unsigned long  ndpi_pf=0;
static unsigned long  ndpi_pg=0;
static unsigned long  ndpi_ph=0;
static unsigned long  ndpi_pi=0;
static unsigned long  ndpi_pi1=0;
static unsigned long  ndpi_pi2=0;
static unsigned long  ndpi_pi3=0;
static unsigned long  ndpi_pi4=0;
static unsigned long  ndpi_pj=0;
static unsigned long  ndpi_pjc=0;
static unsigned long  ndpi_pk=0;

static unsigned long  ndpi_pl[11]={0,};
unsigned long  ndpi_btp_tm[20]={0,};

module_param_named(xt_debug,   ndpi_log_debug, ulong, 0600);
MODULE_PARM_DESC(xt_debug,"Debug level for xt_ndpi (0-3).");
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
module_param_named(lib_trace,  ndpi_lib_trace, ulong, 0600);
MODULE_PARM_DESC(lib_trace,"Debug level for nDPI library (0-off, 1-error, 2-trace, 3-debug, 4->extra debug");
#endif
module_param_named(mtu, ndpi_mtu, ulong, 0600);
MODULE_PARM_DESC(mtu,"Skip checking nonlinear skbuff larger than MTU");

module_param_named(bt_log_size, bt_log_size, ulong, 0400);
MODULE_PARM_DESC(bt_log_size,"Keep information about the lastes N bt-hash. default 0, range: 32 - 512");
module_param_named(bt_hash_size, bt_hash_size, ulong, 0400);
MODULE_PARM_DESC(bt_hash_size,"Hash table size ( *1000 ). default 0, range: 8-32");
module_param_named(bt_hash_timeout, bt_hash_tmo, ulong, 0400);
MODULE_PARM_DESC(bt_hash_timeout,"The expiration time for inactive records in BT-hash (sec). default 1200 range: 900-3600");

module_param_named(max_unk_tcp,max_packet_unk_tcp,ulong, 0600);
module_param_named(max_unk_udp,max_packet_unk_udp,ulong, 0600);
module_param_named(max_unk_other,max_packet_unk_other,ulong, 0600);

module_param_named(ndpi_size_flow_struct,ndpi_size_flow_struct,ulong, 0400);
module_param_named(ndpi_size_id_struct,ndpi_size_id_struct,ulong, 0400);
module_param_named(ndpi_size_hash_ip4p_node,ndpi_size_hash_ip4p_node,ulong, 0400);

module_param_named(err_oversize, ndpi_jumbo, ulong, 0400);
MODULE_PARM_DESC(err_oversize,"Counter nonlinear packets bigger than MTU. [info]");
module_param_named(err_skb_linear, ndpi_falloc, ulong, 0400);
MODULE_PARM_DESC(err_skb_linear,"Counter of unsuccessful conversions of nonlinear packets. [error]");

module_param_named(skb_seg,	 ndpi_nskb, ulong, 0400);
MODULE_PARM_DESC(skb_seg,"Counter nonlinear packets. [info]");
module_param_named(skb_lin,	 ndpi_lskb, ulong, 0400);
MODULE_PARM_DESC(skb_lin,"Counter linear packets. [info]");

module_param_named(flow_created, ndpi_flow_c, ulong, 0400);
MODULE_PARM_DESC(flow_created,"Counter of created flows. [info]");
module_param_named(flow_deleted, ndpi_flow_d, ulong, 0400);
MODULE_PARM_DESC(flow_deleted,"Counter of destroyed flows. [info]");
module_param_named(bt_gc_count,  ndpi_bt_gc, ulong, 0400);

module_param_named(ipv4,         ndpi_p0, ulong, 0400);
module_param_named(ipv6,         ndpi_pa, ulong, 0400);
module_param_named(nonip,        ndpi_pb, ulong, 0400);
module_param_named(err_ip_frag_len, ndpi_p1, ulong, 0400);
module_param_named(err_bad_tcp_udp, ndpi_p2, ulong, 0400);
module_param_named(ct_confirm,   ndpi_p31, ulong, 0400);
module_param_named(err_add_ndpi, ndpi_p34, ulong, 0400);
module_param_named(non_tcpudp,   ndpi_p7, ulong, 0400);
module_param_named(max_parsed_lines, ndpi_p9, ulong, 0400);
module_param_named(id_num,	 ndpi_pc, ulong, 0400);
module_param_named(noncached,	 ndpi_pd, ulong, 0400);
module_param_named(err_prot_err, ndpi_pe, ulong, 0400);
module_param_named(err_prot_err1, ndpi_pf, ulong, 0400);
module_param_named(err_alloc_flow, ndpi_pg, ulong, 0400);
module_param_named(err_alloc_id, ndpi_ph, ulong, 0400);
module_param_named(cached,	 ndpi_pi,  ulong, 0400);
module_param_named(c_ct_not,	 ndpi_pi1, ulong, 0400);
module_param_named(c_skb_not,	 ndpi_pi2, ulong, 0400);
module_param_named(c_all_not,	 ndpi_pi3, ulong, 0400);
module_param_named(c_id_not,	 ndpi_pi4, ulong, 0400);
module_param_named(l4mismatch,	 ndpi_pj,  ulong, 0400);
module_param_named(l4mis_size,	 ndpi_pjc, ulong, 0400);
module_param_named(ndpi_match,	 ndpi_pk,  ulong, 0400);

unsigned long  ndpi_pto=0,
	       ndpi_ptss=0, ndpi_ptsd=0,
	       ndpi_ptds=0, ndpi_ptdd=0,
	       ndpi_ptussf=0,ndpi_ptusdr=0,
	       ndpi_ptussr=0,ndpi_ptusdf=0,
	       ndpi_ptudsf=0,ndpi_ptuddr=0,
	       ndpi_ptudsr=0,ndpi_ptuddf=0 ;
unsigned long 
	       ndpi_pusf=0,ndpi_pusr=0,
	       ndpi_pudf=0,ndpi_pudr=0,
	       ndpi_puo=0;

#include "regexp.c"

static int ndpi_net_id;
static inline struct ndpi_net *ndpi_pernet(struct net *net)
{
	        return net_generic(net, ndpi_net_id);
}

/* detection */
static uint32_t detection_tick_resolution = 1000;

static	enum nf_ct_ext_id nf_ct_ext_id_ndpi = 0;
static	struct kmem_cache *osdpi_flow_cache = NULL;
static	struct kmem_cache *osdpi_id_cache = NULL;
struct kmem_cache *bt_port_cache = NULL;

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
static char *dbl_lvl_txt[5] = {
	"ERR",
	"TRACE",
	"DEBUG",
	"DEBUG2",
	NULL
};
/* debug functions */
static void debug_printf(u_int32_t protocol, void *id_struct, ndpi_log_level_t log_level,
	const char *file_name, const char *func_name, unsigned line_number, const char * format, ...)
{
	struct ndpi_net *n = id_struct ? ((struct ndpi_detection_module_struct *)id_struct)->user_data : NULL;
	if(!n || protocol >= NDPI_NUM_BITS)
		pr_info("ndpi_debug n=%d, p=%u, l=%s\n",n != NULL,protocol,
				log_level < 5 ? dbl_lvl_txt[log_level]:"???");
	if(!n || protocol >= NDPI_NUM_BITS) return;
	
	if(log_level+1 <= ( ndpi_lib_trace < n->debug_level[protocol] ?
				ndpi_lib_trace : n->debug_level[protocol]))  {
		char buf[256];
		const char *short_fn;
        	va_list args;

		memset(buf, 0, sizeof(buf));
        	va_start(args, format);
		vsnprintf(buf, sizeof(buf)-1, format, args);
       		va_end(args);
		short_fn = strrchr(file_name,'/');
		if(!short_fn)
			short_fn = file_name;
		    else
			short_fn++;

		switch(log_level) {
		case NDPI_LOG_ERROR:
                	pr_err("E: P=%d %s:%d:%s %s",protocol, short_fn, line_number, func_name, buf);
			break;
		case NDPI_LOG_TRACE:
                	pr_info("T: P=%d %s:%d:%s %s",protocol, short_fn, line_number, func_name, buf);
			break;
		case NDPI_LOG_DEBUG:
                	pr_info("D: P=%d %s:%d:%s %s",protocol, short_fn, line_number, func_name, buf);
			break;
		case NDPI_LOG_DEBUG_EXTRA:
                	pr_info("D2: P=%d %s:%d:%s %s",protocol, short_fn, line_number, func_name, buf);
			break;
		default:
			;
		}
        }
}

static void set_debug_trace( struct ndpi_net *n) {
	int i;
	const char *t_proto;
	ndpi_debug_function_ptr dbg_printf = (ndpi_debug_function_ptr)NULL;
	if(ndpi_lib_trace)
	    for(i=0; i < NDPI_NUM_BITS; i++) {
		if(!n->mark[i].mark && !n->mark[i].mask) continue;
		t_proto = ndpi_get_proto_by_id(n->ndpi_struct,i);
		if(t_proto) {
			if(!n->debug_level[i]) continue;
			dbg_printf = debug_printf;
			break;
		}
	    }
	if(n->ndpi_struct->ndpi_debug_printf != dbg_printf) {
		pr_info("ndpi: debug message %s\n",dbg_printf != NULL ? "ON":"OFF");
		set_ndpi_debug_function(n->ndpi_struct, dbg_printf);
	} else {
		if(ndpi_log_debug)
		  pr_info("ndpi: debug %s (not changed)\n",
			n->ndpi_struct->ndpi_debug_printf != NULL ? "on":"off");
	}
}
#endif

static uint16_t ndpi_check_ipport(patricia_node_t *node,uint16_t port,int l4);
static char *ct_info(const struct nf_conn * ct,char *buf,size_t buf_size);

#define XCHGP(a,b) { void *__c = a; a = b; b = __c; }

/*
 * simple string collections.
 */

static inline hosts_str_t *str_hosts_alloc(void) {
    return (hosts_str_t *)kcalloc(1,sizeof(hosts_str_t),GFP_KERNEL);
}

static void str_hosts_done(hosts_str_t *h) {
int i;

    if(!h) return;
    for(i=0; i < NDPI_NUM_BITS+1; i++) {
	if(h->p[i]) kfree(h->p[i]);
    }
    kfree(h);
}


static str_collect_t *str_collect_init(size_t num_start) {
str_collect_t *c;

    if(!num_start)
	    num_start =  64;

    c = (str_collect_t *)kmalloc(sizeof(str_collect_t) + num_start + 1, GFP_KERNEL);
    if(!c) 
	return c;

    c->max  = num_start;
    c->last = 0;
    c->s[0] = '\0';
    return c;
}

static  str_collect_t *str_collect_copy(str_collect_t *c,int add_size) {

    str_collect_t *n = str_collect_init(c->last + 1 + add_size);
    if(n) {
	memcpy((char *)n->s,(char *)c->s, c->last + 1 );
	n->last = c->last;
    }
    return n;
}

static  hosts_str_t *str_collect_clone( hosts_str_t *h) {
int i,s0,s1;
hosts_str_t *r;
str_collect_t *t,*n;

    if(!h) return h;
    r = str_hosts_alloc();
    if(!r) return r;

    s0 = s1 = 0;
    for(i=0; i < NDPI_NUM_BITS+1; i++) {
	t = h->p[i];
	if(!t) continue;
	if(!t->last) continue;
	n = str_collect_copy(t,0);
	if(!n) {
		str_hosts_done(r);
		return NULL;
	}
	s0++;
	s1 += n->last;
	r->p[i] = n;
    }
// pr_info("%s: protos %d, strlen sum %d\n",__func__,s0,s1);
    return r;
}

static int str_collect_look(str_collect_t *c,char *str,size_t slen) {
uint32_t ln,i,ni;

if(!c || !str || !slen) return -1;
for(i=0; i < c->last;i = ni) {
	ln = (uint8_t)c->s[i];
	if(!ln) break;
	ni = i+ln+2;
	if(ln == slen && !strncmp(&c->s[i+1],str,slen)) {
		return i;
	}
}
return -1;
}

#if 0
static void str_collect_dump(str_collect_t *c) {
uint32_t ln,i,ni;
char buf[256];
int li = 0;

if(!c) return;

for(i=0; i < c->last;i = ni) {
	ln = (uint8_t)c->s[i];
	if(!ln) break;
	ni = i+ln+2;
	li += snprintf(&buf[li],sizeof(buf)-li-15,"%c%u:%.30s",
			li ? ',':' ',ln,&c->s[i+1]);
	if(li >= sizeof(buf)-15) {
		snprintf(&buf[li],sizeof(buf)-li-1,",...");
		break;
	}
}
pr_info("str_collect_dump '%s' last %lu free %lu last %lu\n",
		buf,c->last, c->max-c->last, c->max);
return;
}
#endif

static char *str_collect_add(str_collect_t **pc,char *str,size_t slen) {
uint32_t nsn;
str_collect_t *nc,*c = *pc;

    if(slen >= 255) {
	pr_err("xt_ndpi: hostname length > 255 chars : %.60s...\n", str);
	return NULL;
    }

    nsn = slen + 3;
    if(nsn < 128) nsn = 128;

    if(c) {
	if(c->last + nsn >= c->max-1) {
		nc = str_collect_copy(c,nsn);
		if(!nc) return NULL;
		kfree(c);
		*pc = nc;
		c = nc;
	}
    } else {
	nc = str_collect_init(nsn);
	if(!nc) return NULL;
	*pc = nc;
	c = nc;
    }

    c->s[c->last] = slen;
    strcpy(&c->s[c->last+1],str);
    str = &c->s[c->last+1];
    c->last += slen + 2;
    c->s[c->last] = '\0';

    return str;
}

static void str_collect_del(str_collect_t *c,char *str, size_t slen) {
uint32_t i,ln,ni;

    if(!c || !str) return;

/* deleting last string ? */
    if(slen < c->last) {
	i = c->last - slen - 2;
	if((uint8_t)c->s[i] == slen && !strcmp(&c->s[i+1],str)) {
		c->s[i] = '\0';
		c->last = i;
		return;
	}
    }
/* check all strings :(  */ 
    i = str_collect_look(c,str,slen);
    if(i >= 0) {
	ln = (uint8_t)c->s[i];
	ni = i+ln+2;
	if(c->last != ni)
		memmove(&c->s[i],&c->s[ni],c->last - ni);
	c->last -= ln + 2;
	c->s[c->last] = '\0';
    }
}

static void *malloc_wrapper(size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}

static void free_wrapper(void *freeable)
{
	kfree(freeable);
}

static void fill_prefix_any(prefix_t *p, union nf_inet_addr *ip,int family) {
	memset(p, 0, sizeof(prefix_t));
	p->ref_count = 0;
	if(family == AF_INET) {
		memcpy(&p->add.sin, ip, 4);
		p->family = AF_INET;
		p->bitlen = 32;
		return;
	}
#ifdef NDPI_DETECTION_SUPPORT_IPV6
	if(family == AF_INET6) {
		memcpy(&p->add.sin, ip, 16);
		p->family = AF_INET6;
		p->bitlen = 128;
	}
#endif
}

static struct ndpi_id_struct *
ndpi_id_search_or_insert(struct ndpi_net *n, 
		union nf_inet_addr *ip)
{
        int res;
        struct osdpi_id_node *this,*id;
	struct rb_root *root;
  	struct rb_node **new, *parent = NULL;

	spin_lock_bh (&n->id_lock);
	root = &n->osdpi_id_root;
	new  = &(root->rb_node);
  	while (*new) {
                this = rb_entry(*new, struct osdpi_id_node, node);
		res = memcmp(ip, &this->ip,sizeof(union nf_inet_addr));

		parent = *new;
  		if (res < 0)
  			new = &((*new)->rb_left);
  		else if (res > 0)
  			new = &((*new)->rb_right);
  		else {
                	kref_get (&this->refcnt);
			spin_unlock_bh (&n->id_lock);
  			return &this->ndpi_id;
		}
  	}
	id = kmem_cache_zalloc (osdpi_id_cache, GFP_ATOMIC);
	if (id == NULL) {
		spin_unlock_bh (&n->id_lock);
		pr_err("xt_ndpi: couldn't allocate new id.\n");
		return NULL;
	}
	(volatile unsigned long int)ndpi_pc++;
	memcpy(&id->ip, ip, sizeof(union nf_inet_addr));
	kref_init (&id->refcnt);

  	rb_link_node(&id->node, parent, new);
  	rb_insert_color(&id->node, root);
	spin_unlock_bh (&n->id_lock);
	return &id->ndpi_id;
}

static void
ndpi_free_id (struct ndpi_net *n, struct osdpi_id_node * id)
{
	if (refcount_dec_and_test(&id->refcnt.refcount)) {
	        rb_erase(&id->node, &n->osdpi_id_root);
	        kmem_cache_free (osdpi_id_cache, id);
		(volatile unsigned long int)ndpi_pc--;
	}
}

#ifdef NF_CT_CUSTOM
static inline struct nf_ct_ext_ndpi *nf_ct_ext_find_ndpi(const struct nf_conn * ct)
{
	return (struct nf_ct_ext_ndpi *)__nf_ct_ext_find(ct,nf_ct_ext_id_ndpi);
}
#else

static inline struct nf_ct_ext_ndpi *nf_ct_ext_find_ndpi(const struct nf_conn * ct)
{
struct nf_ct_ext_labels *l = (struct nf_ct_ext_labels *)__nf_ct_ext_find(ct,nf_ct_ext_id_ndpi);
return l && l->magic == MAGIC_CT ? l->ndpi_ext:NULL;
}

static inline struct nf_ct_ext_labels *nf_ct_ext_find_label(const struct nf_conn * ct)
{
	return (struct nf_ct_ext_labels *)__nf_ct_ext_find(ct,nf_ct_ext_id_ndpi);
}

#endif


#ifdef NF_CT_CUSTOM
static inline void *nf_ct_ext_add_ndpi(struct nf_conn * ct)
{
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
	return nf_ct_ext_add(ct,nf_ct_ext_id_ndpi,GFP_ATOMIC);
  #elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
	return __nf_ct_ext_add_length(ct,nf_ct_ext_id_ndpi,
		sizeof(struct nf_ct_ext_ndpi),GFP_ATOMIC);
  #else
	return __nf_ct_ext_add(ct,nf_ct_ext_id_ndpi,GFP_ATOMIC);
  #endif
}
#endif


static inline void ndpi_init_ct_struct(struct nf_ct_ext_ndpi *ct_ndpi,uint8_t l4_proto) {
	memset((char *)ct_ndpi,0,sizeof(struct nf_ct_ext_ndpi));
	spin_lock_init(&ct_ndpi->lock);
	ct_ndpi->l4_proto = l4_proto;
}

static inline void __ndpi_free_ct_flow(struct nf_ct_ext_ndpi *ct_ndpi) {
	if(ct_ndpi->flow != NULL) {
		ndpi_free_flow(ct_ndpi->flow);
		kmem_cache_free (osdpi_flow_cache, ct_ndpi->flow);
		ct_ndpi->flow = NULL;
		COUNTER(ndpi_flow_d);
		module_put(THIS_MODULE);
	}
}

static int
__ndpi_free_flow (struct nf_conn * ct,void *data) {
	struct ndpi_net *n = data;
	struct nf_ct_ext_ndpi *ct_ndpi = nf_ct_ext_find_ndpi(ct);
	
	if(!ct_ndpi) return 1;

	spin_lock_bh (&n->id_lock);
	if(ct_ndpi->src) {
		ndpi_free_id (n, container_of(ct_ndpi->src,struct osdpi_id_node,ndpi_id ));
		ct_ndpi->src = NULL;
	}
	if(ct_ndpi->dst) {
		ndpi_free_id (n, container_of(ct_ndpi->dst,struct osdpi_id_node,ndpi_id ));
		ct_ndpi->dst = NULL;
	}
	if(ct_ndpi->host) {
		kfree(ct_ndpi->host);
		ct_ndpi->host = NULL;
	}
	if(ct_ndpi->ssl) {
		kfree(ct_ndpi->ssl);
		ct_ndpi->ssl = NULL;
	}
	spin_unlock_bh (&n->id_lock);
	__ndpi_free_ct_flow(ct_ndpi);
	return 1;
}

static void
nf_ndpi_free_flow (struct nf_conn * ct)
{
	struct ndpi_net *n;
	struct nf_ct_ext_ndpi *ct_ndpi = nf_ct_ext_find_ndpi(ct);

	if(!ct_ndpi) return;
	if(ndpi_log_debug > 2) {
	    char ct_buf[128];
	    printk("Free_ct ct_ndpi %p ct %p %s\n",
					(void *)ct_ndpi,(void *)ct,
					ct_info(ct,ct_buf,sizeof(ct_buf)));
	}
	n = ndpi_pernet(nf_ct_net(ct));
	spin_lock_bh(&ct_ndpi->lock);
	__ndpi_free_flow(ct,(void *)n);
	spin_unlock_bh(&ct_ndpi->lock);
#ifndef NF_CT_CUSTOM
	{
	struct nf_ct_ext_labels *ext_l = nf_ct_ext_find_label(ct);
	if(ext_l && ext_l->ndpi_ext && ext_l->magic == MAGIC_CT) {
		ext_l->magic = 0;
		free_wrapper(ext_l->ndpi_ext);
		ext_l->ndpi_ext = NULL;
	}
	}
#endif
}

/* must be locked ct_ndpi->lock */
static struct ndpi_flow_struct * 
ndpi_alloc_flow (struct nf_ct_ext_ndpi *ct_ndpi)
{
        struct ndpi_flow_struct *flow;

        flow = kmem_cache_zalloc (osdpi_flow_cache, GFP_ATOMIC);
        if (flow == NULL) {
                pr_err("xt_ndpi: couldn't allocate new flow.\n");
                return flow;
        }

	ct_ndpi->proto = proto_null;
	ct_ndpi->flow = flow;
	__module_get(THIS_MODULE);
	COUNTER(ndpi_flow_c);
        return flow;
}
#ifndef NF_CT_CUSTOM
static void (*ndpi_nf_ct_destroy)(struct nf_conntrack *) __rcu __read_mostly;

static void ndpi_destroy_conntrack(struct nf_conntrack *nfct) {
	struct nf_conn *ct = (struct nf_conn *)nfct;
	void (*destroy)(struct nf_conntrack *);

	nf_ndpi_free_flow(ct);

	rcu_read_lock();
        destroy = rcu_dereference(ndpi_nf_ct_destroy);
        if(destroy) destroy(nfct);
        rcu_read_unlock();
}
#endif

/*****************************************************************/

static void
ndpi_enable_protocols (struct ndpi_net *n)
{
        int i,c=0;

        spin_lock_bh (&n->ipq_lock);
	if(atomic_inc_return(&n->protocols_cnt[0]) == 1) {
		for (i = 1,c=0; i < NDPI_NUM_BITS; i++) {
			if(!ndpi_get_proto_by_id(n->ndpi_struct,i))
				continue;
			if(!n->mark[i].mark && !n->mark[i].mask)
				continue;
			NDPI_ADD_PROTOCOL_TO_BITMASK(n->protocols_bitmask, i);
			c++;
		}
		ndpi_set_protocol_detection_bitmask2(n->ndpi_struct,
				&n->protocols_bitmask);
	}
	spin_unlock_bh (&n->ipq_lock);
}


static void add_stat(unsigned long int n) {

	if(n > ndpi_p9) ndpi_p9 = n;
	n /= 10;
	if(n < 0) n = 0;
	if(n > sizeof(ndpi_pl)/sizeof(ndpi_pl[0])-1)
		n = sizeof(ndpi_pl)/sizeof(ndpi_pl[0])-1;
	ndpi_pl[n]++;
}

static char *ct_info(const struct nf_conn * ct,char *buf,size_t buf_size) {
 const struct nf_conntrack_tuple *t = &ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;

 snprintf(buf,buf_size,"proto %u %pi4:%d -> %pi4:%d",
		t->dst.protonum,
		&t->src.u3.ip, ntohs(t->src.u.all),
		&t->dst.u3.ip, ntohs(t->dst.u.all));
 return buf;
}

static void packet_trace(const struct sk_buff *skb,const struct nf_conn * ct, char *msg) {
  const struct iphdr *iph = ip_hdr(skb);
  if(iph && iph->version == 4) {
	if(iph->protocol == IPPROTO_TCP || iph->protocol == IPPROTO_UDP) {
		 struct udphdr *udph = (struct udphdr *)(((const u_int8_t *) iph) + iph->ihl * 4); 
		 printk("%s skb %p ct %p proto %d %pi4:%d -> %pi4:%d len %d\n",
			msg ? msg:"",(void *)skb,(void *)ct,
			iph->protocol,&iph->saddr,htons(udph->source),
			&iph->daddr,htons(udph->dest),skb->len);
  	} else
		 printk("%s skb %p ct %p proto %d %pi4 -> %pi4 len %d\n",
			msg ? msg:"",(void *)skb,(void *)ct,
			iph->protocol,&iph->saddr, &iph->daddr,skb->len);
  }
}

static int check_known_ipv4_service( struct ndpi_net *n,
		union nf_inet_addr *ipaddr, uint16_t port, uint8_t protocol) {

	prefix_t ipx;
	patricia_node_t *node;
	uint16_t app_protocol = NDPI_PROTOCOL_UNKNOWN;
	fill_prefix_any(&ipx,ipaddr,AF_INET);

	spin_lock_bh (&n->ipq_lock);
	node = ndpi_patricia_search_best(n->ndpi_struct->protocols_ptree,&ipx);
	if(node) {
	    if(protocol == IPPROTO_UDP || protocol == IPPROTO_TCP)
		app_protocol = ndpi_check_ipport(node,port,protocol == IPPROTO_TCP);
	}
	spin_unlock_bh (&n->ipq_lock);
	return app_protocol;
}

static u32
ndpi_process_packet(struct ndpi_net *n, struct nf_conn * ct, struct nf_ct_ext_ndpi *ct_ndpi,
		    const uint64_t time,
                    const struct sk_buff *skb,int dir)
{
	ndpi_protocol proto = NDPI_PROTOCOL_NULL;
        struct ndpi_id_struct *src, *dst;
        struct ndpi_flow_struct * flow;
	uint32_t low_ip, up_ip, tmp_ip;
	uint16_t low_port, up_port, tmp_port, protocol;
	const struct iphdr *iph = NULL;
#ifdef NDPI_DETECTION_SUPPORT_IPV6
	const struct ipv6hdr *ip6h;

	ip6h = ipv6_hdr(skb);
	if(ip6h && ip6h->version != 6) ip6h = NULL;
#endif
	iph = ip_hdr(skb);

	if(iph && iph->version != 4) iph = NULL;

	if(!iph
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		&& !ip6h
#endif
	  ) {
		COUNTER(ndpi_pf);
		return NDPI_PROCESS_ERROR;
	}

	flow = ct_ndpi->flow;
	if (!flow) {
		flow = ndpi_alloc_flow(ct_ndpi);
		if (!flow) {
			COUNTER(ndpi_pg);
			return NDPI_PROCESS_ERROR;
		}
	}

	src = ct_ndpi->src;
	if (!src) {
		src = ndpi_id_search_or_insert (n,
			&ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u3);
		if (!src) {
			COUNTER(ndpi_ph);
			return NDPI_PROCESS_ERROR;
		}
		ct_ndpi->src = src;
	}
	dst = ct_ndpi->dst;
	if (!dst) {
		dst = ndpi_id_search_or_insert (n,
			&ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u3);
		if (!dst) {
			COUNTER(ndpi_ph);
			return NDPI_PROCESS_ERROR;
		}
		ct_ndpi->dst = dst;
	}

	/* here the actual detection is performed */
	if(dir) {
		src = ct_ndpi->dst;
		dst = ct_ndpi->src;
	}

	flow->packet_direction = dir;
	if(ndpi_log_debug > 1)
		packet_trace(skb,ct,"process    ");
	proto = ndpi_detection_process_packet(n->ndpi_struct,flow,
#ifdef NDPI_DETECTION_SUPPORT_IPV6
				ip6h ?	(uint8_t *) ip6h :
#endif
					(uint8_t *) iph, 
					 skb->len, time, src, dst);

	if(proto.master_protocol == NDPI_PROTOCOL_UNKNOWN && 
	          proto.app_protocol == NDPI_PROTOCOL_UNKNOWN ) {
#ifdef NDPI_DETECTION_SUPPORT_IPV6
	    if(ip6h) {
		low_ip = 0;
		up_ip = 0;
		protocol = ip6h->nexthdr;
	    } else
#endif
	    {
		low_ip=ntohl(iph->saddr);
		up_ip=ntohl(iph->daddr);
		protocol = iph->protocol;
	    }

	    if(protocol == IPPROTO_TCP || protocol == IPPROTO_UDP) {
		low_port = htons(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u.tcp.port);
		up_port  = htons(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u.tcp.port);
	    } else {
		low_port = up_port = 0;
	    }
	    if (iph && flow && flow->packet_counter < 3 &&
			!flow->protocol_id_already_guessed) {
		proto.app_protocol = check_known_ipv4_service(n,
				&ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.u3,up_port,protocol);
		if(proto.app_protocol != NDPI_PROTOCOL_UNKNOWN)
			proto.app_protocol = check_known_ipv4_service(n,
				&ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u3,low_port,protocol);
		if(proto.app_protocol != NDPI_PROTOCOL_UNKNOWN)
			flow->protocol_id_already_guessed = 1;
	    }
	    if(proto.app_protocol == NDPI_PROTOCOL_UNKNOWN) {
		if(low_ip > up_ip) { tmp_ip = low_ip; low_ip=up_ip; up_ip = tmp_ip; }
		if(low_port > up_port) { tmp_port = low_port; low_port=up_port; up_port = tmp_port; }
		proto = ndpi_guess_undetected_protocol (
				n->ndpi_struct,protocol,low_ip,low_port,up_ip,up_port);
	    }
	} else {
		add_stat(flow->packet.parsed_lines);
	}
	if( proto.app_protocol != NDPI_PROTOCOL_UNKNOWN ||
	    proto.master_protocol != NDPI_PROTOCOL_UNKNOWN)
		ct_ndpi->proto = proto;

	return proto.app_protocol;
}
static inline int can_handle(const struct sk_buff *skb,uint8_t *l4_proto)
{
	const struct iphdr *iph;
	uint32_t l4_len;
	uint8_t proto;
#ifdef NDPI_DETECTION_SUPPORT_IPV6
	const struct ipv6hdr *ip6h;

	ip6h = ipv6_hdr(skb);
	if(ip6h->version == 6) {
		COUNTER(ndpi_pa);
		*l4_proto = ip6h->nexthdr;
		// FIXME!
		return 1;
	}
#endif
	iph = ip_hdr(skb);
        if(!iph) { /* not IP */
		COUNTER(ndpi_pb); return 0;
	}
	if(iph->version != 4) {
		COUNTER(ndpi_pb); return 0;
	}
	*l4_proto = proto = iph->protocol;
	COUNTER(ndpi_p0);

	if(ntohs(iph->frag_off) & 0x3fff) {
		COUNTER(ndpi_p1); return 0;
	}
	if(skb->len <= (iph->ihl << 2)) {
		COUNTER(ndpi_p1); return 0; 
	}

	l4_len = skb->len - (iph->ihl << 2);
        if(proto == IPPROTO_TCP) {
		if(l4_len < sizeof(struct tcphdr)) {
			COUNTER(ndpi_p2); return 0;
		}
		return 1;
	}
        if(proto == IPPROTO_UDP) {
		if(l4_len < sizeof(struct udphdr)) {
			COUNTER(ndpi_p2); return 0;
		}
		return 1;
	}
	COUNTER(ndpi_p7);
	return 1;
}

static bool ndpi_host_match( const struct xt_ndpi_mtinfo *info,
			     struct nf_ct_ext_ndpi *ct_ndpi) {
const char *name;
bool res = false;

if(!info->hostname[0]) return true;

do {
  if(info->host) {
	if(!ct_ndpi->host && ct_ndpi->flow) {
		name = ct_ndpi->flow->host_server_name;
		if(*name)
		  ct_ndpi->host = kstrdup(name, GFP_KERNEL);
	}
	if(ct_ndpi->host) {
		res = info->re ? regexec(info->reg_data,ct_ndpi->host) != 0 :
			strstr(ct_ndpi->host,info->hostname) != NULL;
		if(res) break;
	}
  }

  if(info->ssl && ( ct_ndpi->proto.app_protocol == NDPI_PROTOCOL_SSL ||
		    ct_ndpi->proto.master_protocol == NDPI_PROTOCOL_SSL )) {
	if(!ct_ndpi->ssl && ct_ndpi->flow) {
		name = ct_ndpi->flow->protos.stun_ssl.ssl.server_certificate;
		if(*name)
		  ct_ndpi->ssl = kstrdup(name, GFP_KERNEL);
	}
	if(ct_ndpi->ssl) {
		res = info->re ? regexec(info->reg_data,ct_ndpi->ssl) != 0 :
			strstr(ct_ndpi->ssl,info->hostname) != NULL;
		if(res) break;
	}
  }
} while(0);

if(ndpi_log_debug > 2)
    printk("%s: match%s %s %s '%s' %s,%s %d\n", __func__,
	info->re ? "-re":"", info->host ? "host":"", info->ssl ? "ssl":"",
	info->hostname,ct_ndpi->host ? ct_ndpi->host:"-",
	ct_ndpi->ssl ? ct_ndpi->ssl:"-",res);

return res;
}

#define NDPI_ID 0x44504900ul

#define pack_proto(proto) ((proto.app_protocol << 16) | proto.master_protocol)

static bool
ndpi_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	uint32_t r_proto;
	ndpi_protocol proto = NDPI_PROTOCOL_NULL;
	uint64_t time;
	const struct xt_ndpi_mtinfo *info = par->matchinfo;

	enum ip_conntrack_info ctinfo;
	struct nf_conn * ct;
	struct timespec tm;
	struct sk_buff *linearized_skb = NULL;
	const struct sk_buff *skb_use = NULL;
	struct nf_ct_ext_ndpi *ct_ndpi = NULL;
	struct ndpi_cb *c_proto;
	uint8_t l4_proto=0;
	bool result=false, host_match = true;

	char ct_buf[128];

	proto.app_protocol = NDPI_PROCESS_ERROR;

	c_proto = (void *)&skb->cb[sizeof(skb->cb)-sizeof(struct ndpi_cb)];

    do {
	if(c_proto->data[0] == NDPI_ID &&
	   c_proto->data[1] == NDPI_PROCESS_ERROR) {
		break;
	}
	if(!can_handle(skb,&l4_proto)) {
		proto.app_protocol = NDPI_PROTOCOL_UNKNOWN;
		break;
	}
	if( skb->len > ndpi_mtu && skb_is_nonlinear(skb) ) {
		COUNTER(ndpi_jumbo);
		break;
	}

	COUNTER(ndpi_pk);

	ct = nf_ct_get (skb, &ctinfo);
	if (ct == NULL) {
		COUNTER(ndpi_p31);
		if(ndpi_log_debug > 2)
			printk("nf_ct_get(%p) NULL\n",(void *)skb);
		break;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,12,0)
	if (nf_ct_is_untracked(ct))
#else
	if(ctinfo == IP_CT_UNTRACKED)	
#endif
	{
		COUNTER(ndpi_p31);
		break;
	}

#ifdef NF_CT_CUSTOM
	ct_ndpi = nf_ct_ext_find_ndpi(ct);
	if(!ct_ndpi) {
		if(nf_ct_is_confirmed(ct)) {
			COUNTER(ndpi_p31);
			break;
		}
		ct_ndpi = nf_ct_ext_add_ndpi(ct);
		if(ndpi_log_debug > 2)
			printk("Create  ct_ndpi %p ct %p %s\n",
					(void *)ct_ndpi, (void *)ct,
					ct_info(ct,ct_buf,sizeof(ct_buf)));
		if(ct_ndpi) {
			ndpi_init_ct_struct(ct_ndpi,l4_proto);
		} else {
			COUNTER(ndpi_p34);
			break;
		}
	} else
	    if(ndpi_log_debug > 2)
		printk("Reuse   ct_ndpi %p ct %p %s\n",
				(void *)ct_ndpi, (void *)ct,
				ct_info(ct,ct_buf,sizeof(ct_buf)));
#else
	{
	    struct nf_ct_ext_labels *ct_label = nf_ct_ext_find_label(ct);
	    if(ct_label) {
		if(!ct_label->magic) {
			ct_ndpi = (struct nf_ct_ext_ndpi *)malloc_wrapper(sizeof(struct nf_ct_ext_ndpi));
			if(ct_ndpi) {
				ct_label->magic = MAGIC_CT;
				ct_label->ndpi_ext = ct_ndpi;
				ndpi_init_ct_struct(ct_ndpi,l4_proto);
				if(ndpi_log_debug > 2)
					printk("Create  ct_ndpi %p ct %p %s\n",
						(void *)ct_ndpi, (void *)ct, ct_info(ct,ct_buf,sizeof(ct_buf)));
			}
		} else {
			if(ct_label->magic == MAGIC_CT) {
				ct_ndpi = ct_label->ndpi_ext;
				if(ndpi_log_debug > 2)
					printk("Reuse   ct_ndpi %p ct %p %s\n",
						(void *)ct_ndpi, (void *)ct, ct_info(ct,ct_buf,sizeof(ct_buf)));
			  } else
				COUNTER(ndpi_p34);
		}
	    } else 
		COUNTER(ndpi_p31);
	}
#endif
	if(!ct_ndpi)
		break;

	proto.app_protocol = NDPI_PROTOCOL_UNKNOWN;
	if(ndpi_log_debug > 3)
		packet_trace(skb,ct,"Start      ");

	spin_lock_bh (&ct_ndpi->lock);


	if( c_proto->data[0] == NDPI_ID ) {
	    if(c_proto->last_ct == ct) {
		proto = ct_ndpi->proto;
		if(info->hostname[0])
			host_match = ndpi_host_match(info,ct_ndpi);

		spin_unlock_bh (&ct_ndpi->lock);
		COUNTER(ndpi_pi);
		if(ndpi_log_debug > 1)
		    packet_trace(skb,ct,"cache      ");
		break;
	    }
	    if(c_proto->last_ct != ct)
		    	COUNTER(ndpi_pi3);
	} else
		COUNTER(ndpi_pi4);

	/* don't pass icmp for TCP/UDP to ndpi_process_packet()  */
	if(l4_proto == IPPROTO_ICMP && ct_ndpi->l4_proto != IPPROTO_ICMP) {
		proto.master_protocol = NDPI_PROTOCOL_IP_ICMP;
		proto.app_protocol = NDPI_PROTOCOL_IP_ICMP;
		spin_unlock_bh (&ct_ndpi->lock);
		COUNTER(ndpi_pj);
		ndpi_pjc += skb->len;
		break;
	}
#ifdef NDPI_DETECTION_SUPPORT_IPV6
	if(l4_proto == IPPROTO_ICMPV6 && ct_ndpi->l4_proto != IPPROTO_ICMPV6) {
		proto.master_protocol = NDPI_PROTOCOL_IP_ICMPV6;
		proto.app_protocol = NDPI_PROTOCOL_IP_ICMPV6;
		spin_unlock_bh (&ct_ndpi->lock);
		COUNTER(ndpi_pj);
		ndpi_pjc += skb->len;
		break;
	}
#endif
	if(ct_ndpi->detect_done) {
		proto = ct_ndpi->proto;
		c_proto->data[0] = NDPI_ID;
		c_proto->data[1] = pack_proto(proto);
		c_proto->last_ct = ct;

		if(info->hostname[0])
			host_match = ndpi_host_match(info,ct_ndpi);

		spin_unlock_bh (&ct_ndpi->lock);
		if(ndpi_log_debug > 1)
			packet_trace(skb,ct,"detect_done ");
		break;
	}
	if(ct_ndpi->proto.app_protocol == NDPI_PROTOCOL_UNKNOWN ||
	    ct_ndpi->flow) {
		struct ndpi_net *n;

		if (skb_is_nonlinear(skb)) {
			linearized_skb = skb_copy(skb, GFP_ATOMIC);
			if (linearized_skb == NULL) {
				spin_unlock_bh (&ct_ndpi->lock);
				COUNTER(ndpi_falloc);
				proto.app_protocol = NDPI_PROCESS_ERROR;
				break;
			}
			skb_use = linearized_skb;
			ndpi_nskb += 1;
		} else {
			skb_use = skb;
			ndpi_lskb += 1;
		}

		getnstimeofday(&tm);
		time = ((uint64_t) tm.tv_sec) * detection_tick_resolution +
			(uint32_t)tm.tv_nsec / (1000000000ul / detection_tick_resolution);

		n = ndpi_pernet(nf_ct_net(ct));
		r_proto = ndpi_process_packet(n, ct,
				ct_ndpi, time, skb_use,
				CTINFO2DIR(ctinfo) != IP_CT_DIR_ORIGINAL);

		c_proto->data[0] = NDPI_ID;
		c_proto->data[1] = r_proto;
		c_proto->last_ct = ct;
		COUNTER(ndpi_pd);


		if(r_proto == NDPI_PROCESS_ERROR) {
			// special case for errors
			COUNTER(ndpi_pe);
			c_proto->data[1] = r_proto;
			proto.app_protocol = r_proto;
			proto.master_protocol = NDPI_PROTOCOL_UNKNOWN;
			if(ct_ndpi->proto.app_protocol == NDPI_PROTOCOL_UNKNOWN) {
				ct_ndpi->proto.app_protocol = r_proto;
			}
		} else {
			if(info->hostname[0])
				host_match = ndpi_host_match(info,ct_ndpi);
			if(r_proto != NDPI_PROTOCOL_UNKNOWN) {
				proto = ct_ndpi->proto;
				c_proto->data[1] = pack_proto(proto);
				if(proto.app_protocol != NDPI_PROTOCOL_UNKNOWN)
					atomic_inc(&n->protocols_cnt[proto.app_protocol]);
				if(proto.master_protocol != NDPI_PROTOCOL_UNKNOWN)
					atomic_inc(&n->protocols_cnt[proto.master_protocol]);
			} else { // unknown
				if(ct_ndpi->proto.app_protocol != NDPI_PROTOCOL_UNKNOWN &&
				   ct_ndpi->flow->no_cache_protocol) { // restore proto
					proto = ct_ndpi->proto;
					c_proto->data[1] = pack_proto(proto);
				} else {
					switch(ct_ndpi->l4_proto) {
					  case IPPROTO_TCP:
						  if(ct_ndpi->flow->packet_counter > max_packet_unk_tcp)
							  ct_ndpi->detect_done = 1;
						  break;
					  case IPPROTO_UDP:
						  if(ct_ndpi->flow->packet_counter > max_packet_unk_udp)
							  ct_ndpi->detect_done = 1;
						  break;
					  default:
						  if(ct_ndpi->flow->packet_counter > max_packet_unk_other)
							  ct_ndpi->detect_done = 1;
					}
					if(ct_ndpi->detect_done && ct_ndpi->flow)
						__ndpi_free_ct_flow(ct_ndpi);
				}
			}
		}
		spin_unlock_bh (&ct_ndpi->lock);

		if(linearized_skb != NULL)
			kfree_skb(linearized_skb);
	}
    } while(0);

    if (info->error)
	return (proto.app_protocol == NDPI_PROCESS_ERROR) ^ (info->invert != 0);

    do {
	if (info->have_master) {
		result = proto.master_protocol != NDPI_PROTOCOL_UNKNOWN;
		break;
	}
	if(info->empty) {
		result = true;
		break;
	}
	if (info->m_proto && !info->p_proto) {
		result = NDPI_COMPARE_PROTOCOL_TO_BITMASK(info->flags,proto.master_protocol) != 0;
		break;
	}

	if (!info->m_proto && info->p_proto) {
		result = NDPI_COMPARE_PROTOCOL_TO_BITMASK(info->flags,proto.app_protocol) != 0 ;
		break;
	}

	if (proto.app_protocol != NDPI_PROTOCOL_UNKNOWN) {
		result = NDPI_COMPARE_PROTOCOL_TO_BITMASK(info->flags,proto.app_protocol) != 0;
		if(proto.master_protocol !=  NDPI_PROTOCOL_UNKNOWN)
			result |= NDPI_COMPARE_PROTOCOL_TO_BITMASK(info->flags,proto.master_protocol) != 0;
		break;
	}
	result = NDPI_COMPARE_PROTOCOL_TO_BITMASK(info->flags,proto.master_protocol) != 0;
    } while(0);
    return ( result & host_match ) ^ (info->invert != 0);
}


static int
ndpi_mt_check(const struct xt_mtchk_param *par)
{
struct xt_ndpi_mtinfo *info = par->matchinfo;

	if (!info->error &&  !info->have_master && !info->hostname[0] &&
	     NDPI_BITMASK_IS_ZERO(info->flags)) {
		pr_info("No selected protocols.\n");
		return -EINVAL;
	}
	info->empty = NDPI_BITMASK_IS_ZERO(info->flags);
	if(info->hostname[0] && info->re) {
		char re_buf[sizeof(info->hostname)];
		int re_len = strlen(info->hostname);
		if(re_len < 3 || info->hostname[0] != '/' || 
				info->hostname[re_len-1] != '/') {
			pr_info("Invalid REGEXP\n");
			return -EINVAL;
		}
		re_len -= 2;
		strncpy(re_buf,&info->hostname[1],re_len);
		re_buf[re_len] = '\0';
		info->reg_data = regcomp(re_buf,&re_len);
		if(!info->reg_data) {
			pr_info("regcomp failed\n");
			return -EINVAL;
		}
		if(ndpi_log_debug > 2)
			pr_info("regcomp '%s' success\n",re_buf);
	} else {
		info->reg_data = NULL;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	{
		int ret;

		ret = nf_ct_netns_get(par->net, par->family);
		if (ret < 0) {
			pr_info("cannot load conntrack support for proto=%u\n",
				par->family);
			return ret;
		}
	}
#endif
	ndpi_enable_protocols (ndpi_pernet(par->net));
	return 0;
}

static void 
ndpi_mt_destroy (const struct xt_mtdtor_param *par)
{
struct xt_ndpi_mtinfo *info = par->matchinfo;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	nf_ct_netns_put(par->net, par->family);
#endif
	if(info->reg_data) kfree(info->reg_data);
}

#ifdef NF_CT_CUSTOM

char *ndpi_proto_to_str(char *buf,size_t size,ndpi_protocol *p,ndpi_mod_str_t *ndpi_str)
{
const char *t_app,*t_mast;
buf[0] = '\0';
t_app = ndpi_get_proto_by_id(ndpi_str,p->app_protocol);
t_mast= ndpi_get_proto_by_id(ndpi_str,p->master_protocol);
if(p->app_protocol && t_app)
	strncpy(buf,t_app,size);
if(p->master_protocol && t_mast) {
	strncat(buf,",",size);
	strncat(buf,t_mast,size);
}
return buf;
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,8,0)
static unsigned int seq_print_ndpi(struct seq_file *s,
					  const struct nf_conn *ct,
					  int dir)
{

       struct nf_ct_ext_ndpi *ct_ndpi;
       char res_str[64];
       ndpi_mod_str_t *ndpi_str;
       if(dir != IP_CT_DIR_REPLY) return 0;
	
       ct_ndpi = nf_ct_ext_find_ndpi(ct);
       ndpi_str = ndpi_pernet(nf_ct_net(ct))->ndpi_struct;
       if(ct_ndpi && (ct_ndpi->proto.app_protocol || ct_ndpi->proto.master_protocol))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0)
	    seq_printf(s,"ndpi=%s ",ndpi_proto_to_str(res_str,sizeof(res_str),&ct_ndpi->proto,ndpi_str));
#else
	    return seq_printf(s,"ndpi=%s ",
			    ndpi_proto_to_str(res_str,sizeof(res_str),&ct_ndpi->proto,ndpi_str));
#endif
       return 0;
}
#endif
#endif

static void ndpi_proto_markmask(struct ndpi_net *n, u_int32_t *var,
		ndpi_protocol *proto, int mode)
{
    if(mode == 1) {
	if(proto->master_protocol < NDPI_NUM_BITS) {
		*var &= ~n->mark[proto->master_protocol].mask;
		*var |=  n->mark[proto->master_protocol].mark;
	}
	return;
    }
    if(mode == 2) {
	if(proto->app_protocol < NDPI_NUM_BITS) {
		*var &= ~n->mark[proto->app_protocol].mask;
		*var |=  n->mark[proto->app_protocol].mark;
	}
	return;
    }
    if(proto->master_protocol != NDPI_PROTOCOL_UNKNOWN) {
	if(proto->master_protocol < NDPI_NUM_BITS) {
		*var &= ~n->mark[proto->master_protocol].mask;
		*var |=  n->mark[proto->master_protocol].mark;
	}
    }
    if(proto->app_protocol != NDPI_PROTOCOL_UNKNOWN) {
	if(proto->app_protocol < NDPI_NUM_BITS) {
		*var &= ~(n->mark[proto->app_protocol].mask << 16);
		*var |=  n->mark[proto->app_protocol].mark << 16;
	}
    }
}

static unsigned int
ndpi_tg(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct xt_ndpi_tginfo *info = par->targinfo;
	ndpi_protocol proto = NDPI_PROTOCOL_NULL;
	struct ndpi_net *n = ndpi_pernet(dev_net(skb->dev ? : skb_dst(skb)->dev));
	int mode = 0;

	if(info->p_proto_id || info->m_proto_id || info->any_proto_id) {
		enum ip_conntrack_info ctinfo;
		struct nf_conn * ct;
		struct nf_ct_ext_ndpi *ct_ndpi;

		ct = nf_ct_get (skb, &ctinfo);
		if(ct) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,12,0)
			if (!nf_ct_is_untracked(ct))
#else
			if(ctinfo != IP_CT_UNTRACKED)
#endif
			{
			    ct_ndpi = nf_ct_ext_find_ndpi(ct);
			    if(ct_ndpi) {
				spin_lock_bh (&ct_ndpi->lock);
				proto = ct_ndpi->proto;
				spin_unlock_bh (&ct_ndpi->lock);
			    }
			}
		}
		if(info->m_proto_id) mode |= 1;
		if(info->p_proto_id) mode |= 2;
		if(info->any_proto_id) mode |= 3;
	}

	if(info->t_mark) {
	        skb->mark = (skb->mark & ~info->mask) | info->mark;
		if(mode)
			ndpi_proto_markmask(n,&skb->mark,&proto,mode);
	}
	if(info->t_clsf) {
	        skb->priority = (skb->priority & ~info->mask) | info->mark;
		if(mode)
			ndpi_proto_markmask(n,&skb->priority,&proto,mode);
	}
        return info->t_accept ? NF_ACCEPT : XT_CONTINUE;
}

static int
ndpi_tg_check(const struct xt_tgchk_param *par)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	{
		int ret;

		ret = nf_ct_netns_get(par->net, par->family);
		if (ret < 0) {
			pr_info("cannot load conntrack support for proto=%u\n",
				par->family);
			return ret;
		}
	}
#endif
        ndpi_enable_protocols (ndpi_pernet(par->net));
	return nf_ct_l3proto_try_module_get (par->family);
}

static void 
ndpi_tg_destroy (const struct xt_tgdtor_param *par)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	nf_ct_netns_put(par->net, par->family);
#endif
	nf_ct_l3proto_module_put (par->family);
}


static struct xt_match
ndpi_mt_reg __read_mostly = {
	.name = "ndpi",
	.revision = 0,
#ifdef NDPI_DETECTION_SUPPORT_IPV6
        .family = NFPROTO_UNSPEC,
#else
	.family = NFPROTO_IPV4,
#endif
	.match = ndpi_mt,
	.checkentry = ndpi_mt_check,
	.destroy = ndpi_mt_destroy,
	.matchsize = XT_ALIGN(sizeof(struct xt_ndpi_mtinfo)),
	.me = THIS_MODULE,
};

static struct xt_target ndpi_tg_reg __read_mostly = {
        .name           = "NDPI",
        .revision       = 0,
#ifdef NDPI_DETECTION_SUPPORT_IPV6
        .family         = NFPROTO_UNSPEC,
#else
	.family		= NFPROTO_IPV4,
#endif
        .target         = ndpi_tg,
	.checkentry	= ndpi_tg_check,
	.destroy	= ndpi_tg_destroy,
        .targetsize     = sizeof(struct xt_ndpi_tginfo),
        .me             = THIS_MODULE,
};
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
static void bt_port_gc(struct timer_list *t) {
	struct ndpi_net *n = from_timer(n, t, gc);
#else
static void bt_port_gc(unsigned long data) {
        struct ndpi_net *n = (struct ndpi_net *)data;
#endif
        struct ndpi_detection_module_struct *ndpi_struct = n->ndpi_struct;
	struct hash_ip4p_table *ht = ndpi_struct->bt_ht;
#ifdef NDPI_DETECTION_SUPPORT_IPV6
	struct hash_ip4p_table *ht6 = ndpi_struct->bt6_ht;
#endif
	struct timespec tm;
	int i;

	if(!ht
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		&& !ht6
#endif
		)  return;

	getnstimeofday(&tm);
	spin_lock(&ht->lock);
	/* full period 64 seconds */
	for(i=0; i < ht->size/128;i++) {
		if(n->gc_index < 0 ) n->gc_index = 0;
		if(n->gc_index >= ht->size-1) n->gc_index = 0;

		if(ht && ht->tbl[n->gc_index].len)
			n->gc_count += ndpi_bittorrent_gc(ht,n->gc_index,tm.tv_sec);
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		if(ht6) {
		   if(ht6->tbl[n->gc_index].len)
			n->gc_count += ndpi_bittorrent_gc(ht6,n->gc_index,tm.tv_sec);
		}
#endif
		n->gc_index++;
	}
	spin_unlock(&ht->lock);
	
	ndpi_bt_gc = n->gc_count;

	n->gc.expires = jiffies + HZ/2;
	add_timer(&n->gc);
}

static int inet_ntop_port(int family,void *ip, u_int16_t port, char *lbuf, size_t bufsize) {
u_int8_t *ipp = (u_int8_t *)ip;
u_int16_t *ip6p = (u_int16_t *)ip;
return  family == AF_INET6 ?
		snprintf(lbuf,bufsize-1, "%x:%x:%x:%x:%x:%x:%x:%x.%d",
			htons(ip6p[0]),htons(ip6p[1]),htons(ip6p[2]),htons(ip6p[3]),
			htons(ip6p[4]),htons(ip6p[5]),htons(ip6p[6]),htons(ip6p[7]),
			htons(port))
	      :	snprintf(lbuf,bufsize-1, "%d.%d.%d.%d:%d",
			ipp[0],ipp[1],ipp[2],ipp[3],htons(port));
}

static ssize_t _ninfo_proc_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos,int family)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
        struct ndpi_detection_module_struct *ndpi_struct = n->ndpi_struct;
	struct hash_ip4p_table *ht,*ht4 = ndpi_struct->bt_ht;
#ifdef NDPI_DETECTION_SUPPORT_IPV6
	struct hash_ip4p_table *ht6 = ndpi_struct->bt6_ht;
#endif
	char lbuf[128];
	struct hash_ip4p *t;
	size_t p;
	int l;
	ht = 
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		family == AF_INET6 ? ht6:
#endif
		ht4;
	if(!ht) {
	    if(!*ppos) {
	        l =  snprintf(lbuf,sizeof(lbuf)-1, "hash disabled\n");
		if (!(access_ok(VERIFY_WRITE, buf, l) &&
				! __copy_to_user(buf, lbuf, l))) return -EFAULT;
		(*ppos)++;
		return l;
	    }
	    return 0;
	}
	if(n->n_hash < 0 || n->n_hash >= ht->size-1) {
	    int tmin,tmax,i;

	    if(!*ppos) {
		tmin = 0x7fffffff;
		tmax = 0;
		t = &ht->tbl[0];

		for(i = ht->size-1; i >= 0 ;i--,t++) {
			if(t->len > 0 && t->len < tmin) tmin = t->len;
			if(t->len > tmax) tmax = t->len;
		}
		if(!atomic_read(&ht->count)) tmin = 0;
	        l =  snprintf(lbuf,sizeof(lbuf)-1,
			"hash_size %lu hash timeout %lus count %u min %d max %d gc %d\n",
				bt_hash_size*1024,bt_hash_tmo,
				atomic_read(&ht->count),tmin,tmax,n->gc_count	);

		if (!(access_ok(VERIFY_WRITE, buf, l) &&
				! __copy_to_user(buf, lbuf, l))) return -EFAULT;
		(*ppos)++;
		return l;
	    }
	    /* ppos > 0 */
#define BSS1 144
#define BSS2 12
	    if(*ppos * BSS1 >= bt_hash_size*1024) return 0;

	    t = &ht->tbl[(*ppos-1)*BSS1];
	    p=0;
	    for(i=0; i < BSS1;i++,t++) {
		if(!(i % BSS2)) {
		        l = snprintf(lbuf,sizeof(lbuf)-1, "%d:\t",(int)(i+(*ppos-1)*BSS1));
			if (!(access_ok(VERIFY_WRITE, buf+p, l) && !__copy_to_user(buf+p, lbuf, l)))
				return -EFAULT;
			p += l;
		}
	        l = snprintf(lbuf,sizeof(lbuf)-1, "%5zu%c",
				t->len, (i % BSS2) == (BSS2-1) ? '\n':' ');
		
		if (!(access_ok(VERIFY_WRITE, buf+p, l) &&
				!__copy_to_user(buf+p, lbuf, l)))
			return -EFAULT;
		p += l;
	    }
	    (*ppos)++;
	    return p;
	}
	t = &ht->tbl[n->n_hash];
	if(!*ppos) {
	        l =  snprintf(lbuf,sizeof(lbuf)-1, "index %d len %zu\n",
				n->n_hash,t->len);
		if (!(access_ok(VERIFY_WRITE, buf, l) &&
				!__copy_to_user(buf, lbuf, l))) return -EFAULT;
		(*ppos)++;
		return l;
	}
	if(*ppos > 1) return 0;
	p = 0;
	spin_lock(&t->lock);
	do {
		struct hash_ip4p_node *x = t->top;
	 	struct timespec tm;

	        getnstimeofday(&tm);
		while(x && p < count - 128) {
		        l =  inet_ntop_port(family,&x->ip,x->port,lbuf,sizeof(lbuf)-2);
			l += snprintf(&lbuf[l],sizeof(lbuf)-l-1, " %d %x %u\n",
				(int)(tm.tv_sec - x->lchg),x->flag,x->count);

			if (!(access_ok(VERIFY_WRITE, buf+p, l) &&
				!__copy_to_user(buf+p, lbuf, l))) return -EFAULT;
			p += l;
			x = x->next;
		}
	} while(0);
	spin_unlock(&t->lock);
	(*ppos)++;
	return p;
}

static int ninfo_proc_open(struct inode *inode, struct file *file)
{
        return 0;
}
static ssize_t ninfo_proc_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
return _ninfo_proc_read(file,buf,count,ppos,AF_INET);
}

#ifdef NDPI_DETECTION_SUPPORT_IPV6
static ssize_t ninfo6_proc_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
return _ninfo_proc_read(file,buf,count,ppos,AF_INET6);
}
#endif

static ssize_t
ninfo_proc_write(struct file *file, const char __user *buffer,
                     size_t length, loff_t *loff)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
	char buf[32];
	int idx;

        if (length > 0) {
		memset(buf,0,sizeof(buf));
		if (!(access_ok(VERIFY_READ, buffer, length) && 
			!__copy_from_user(&buf[0], buffer, min(length,sizeof(buf)-1))))
			        return -EFAULT;
		if(sscanf(buf,"%d",&idx) != 1) return -EINVAL;
		n->n_hash = idx;
        }
        return length;
}

#ifdef BT_ANNOUNCE
static ssize_t nann_proc_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
        struct ndpi_detection_module_struct *ndpi_struct = n->ndpi_struct;
	struct bt_announce *b = ndpi_struct->bt_ann;
	int  bt_len = ndpi_struct->bt_ann_len;
	char lbuf[512],ipbuf[64];
	int i,l,p;

	for(i = 0,p = 0; i < bt_len; i++,b++) {
		if(!b->time) break;

		if(i < *ppos ) continue;
		if(!b->ip[0] && !b->ip[1] && b->ip[2] == 0xfffffffful)
			inet_ntop_port(AF_INET,&b->ip[3],b->port,ipbuf,sizeof(ipbuf));
		    else
			inet_ntop_port(AF_INET6,&b->ip,b->port,ipbuf,sizeof(ipbuf));
	        l =  snprintf(lbuf,sizeof(lbuf)-1, "%08x%08x%08x%08x%08x %s %u '%.*s'\n",
				htonl(b->hash[0]),htonl(b->hash[1]),
				htonl(b->hash[2]),htonl(b->hash[3]),htonl(b->hash[4]),
				ipbuf,b->time,b->name_len,b->name);

		if(count < l) break;

		if (!(access_ok(VERIFY_WRITE, buf+p, l) &&
				!__copy_to_user(buf+p, lbuf, l))) return -EFAULT;
		p += l;
		count -= l;
		(*ppos)++;
	}
	return p;
}
#endif


/*************************** ip_def **************************/

static uint16_t ndpi_check_ipport(patricia_node_t *node,uint16_t port,int l4) {
struct ndpi_port_def *pd;
if(!node) return NDPI_PROTOCOL_UNKNOWN;
pd = node->data;
if(!pd) return NDPI_PROTOCOL_UNKNOWN;
if(pd->count[l4]) {
	int start,end,i;
	ndpi_port_range_t *pt;

	start = !l4 ? 0:pd->count[0];
	end = start + pd->count[l4];

	if(port >= pd->p[start].start &&
	   port <= pd->p[end-1].end) {
		i = start + ((end-start) >> 1);
		do {
			pt = &pd->p[i];
			if(pt->start <= port && port <= pt->end) return pt->proto;
			if(port < pt->start) {
			    end = i;
			} else {
			    start = i+1;
			}
			i = start + ((end-start) >> 1);
		} while(start < end);
	}
}
/* FIXME */
//return node->value.user_value;
return NDPI_PROTOCOL_UNKNOWN;
}

static int ndpi_print_port_range(ndpi_port_range_t *pt,
		int count,char *buf,size_t bufsize,
		ndpi_mod_str_t *ndpi_str) {
 const char *t_proto;
 int l = 0;
	for(; count > 0; count--,pt++) {
		if(l && l < bufsize) buf[l++] = ' ';
		l += snprintf(&buf[l],bufsize - l,"%s",
			  pt->l4_proto ? "tcp":"udp");
		if(pt->start == pt->end)
		  l += snprintf(&buf[l],bufsize - l,":%d",pt->start);
		else
		  l += snprintf(&buf[l],bufsize - l,":%d-%d",pt->start,pt->end);

		t_proto = ndpi_get_proto_by_id(ndpi_str,pt->proto);
		l += snprintf(&buf[l],bufsize - l,":%s", t_proto ? t_proto : "unknown");
		if(l == bufsize) break;
	}
	return l;
}

static int parse_n_proto(char *pr,ndpi_port_range_t *np,ndpi_mod_str_t *ndpi_str)
{
int i;

if(!*pr) return 1;

if(!strcmp(pr,"any")) {
	np->proto = NDPI_NUM_BITS;
	return 0;
}
i = ndpi_get_proto_by_name(ndpi_str,pr);
if(i != NDPI_PROTOCOL_UNKNOWN) {
	DP("%s: parse_n_proto(%s)=%x\n",pr,i);
	np->proto =i;
	return 0;
}
return 1;
}

static int parse_l4_proto(char *pr,ndpi_port_range_t *np)
{
if(!*pr) return 1;

if(!strcmp(pr,"any")) {
	np->l4_proto = 2; return 0;
}
if(!strcmp(pr,"tcp")) {
	np->l4_proto = 1; return 0;
}
if(!strcmp(pr,"udp")) {
	np->l4_proto = 0; return 0;
}
return 1;
}

static int parse_port_range(char *pr,ndpi_port_range_t *np)
{
    char *d;
    uint32_t v;
    
    if(!*pr) return 1;

    if(!strcmp(pr,"any")) {
	np->start = 0;
	np->end = 0;
	return 0;
    }
    d = strchr(pr,'-');
    if(d) *d++ = '\0';
    
    if(kstrtou32(pr,10,&v)) return 1;
    if(v > 0xffff) return 1;

    np->start = v;

    if(d) {
    	if(kstrtou32(d,10,&v)) return 1;
	if(v > 0xffff) return 1;
	np->end = v;
    } else {
    	np->end = v;
    }
    if(np->start > np->end) {
	    v = np->start;
	    np->start = np->end;
	    np->end = v;
    }
    if(d) *--d = '-';

    return 0;
}

/**************************************************************/
struct ndpi_port_def *ndpi_port_range_replace(
		struct ndpi_port_def *pd,
		int start, int end,
		ndpi_port_range_t *np,
		int count,int proto,
		ndpi_mod_str_t *ndpi_str)
{
struct ndpi_port_def *pd1;

DBGDATA(char dbuf[256])

#ifdef NDPI_IPPORT_DEBUG
ndpi_print_port_range(pd->p,pd->count[0]+pd->count[1],dbuf,sizeof(dbuf),ndpi_str);
DP("%s: old %d,%d %s\n",pd->count[0],pd->count[1],dbuf);
ndpi_print_port_range(np,count,dbuf,sizeof(dbuf),ndpi_str);
DP("%s: on %d,%d,%d,%d %s\n",start,end,count,proto,dbuf);
#endif

if( (end-start) == count) {
	if(count) {
	    memcpy(&pd->p[start],np,count*sizeof(ndpi_port_range_t));
	    DP("%s: replaced!\n");
	}
	return pd;
}

pd1 = ndpi_malloc( sizeof(struct ndpi_port_def) + 
		   sizeof(ndpi_port_range_t) * 
		   (count + pd->count[1-proto]));
if(!pd1) return pd;

memcpy(pd1,pd,sizeof(struct ndpi_port_def));
if(!proto) { // udp
	if(count) 
		memcpy( &pd1->p[0],np,count*sizeof(ndpi_port_range_t));
	if(pd->count[1])
		memcpy( &pd1->p[pd->count[0]], &pd->p[pd->count[0]],
			pd->count[1]*sizeof(ndpi_port_range_t));
	pd1->count[0] = count;
} else { // tcp
	if(pd->count[0])
		memcpy( &pd1->p[0], &pd->p[0],
			pd->count[0]*sizeof(ndpi_port_range_t));
	if(count)
		memcpy( &pd1->p[pd->count[0]], np,
			count*sizeof(ndpi_port_range_t));
	pd1->count[1] = count;
}

#ifdef NDPI_IPPORT_DEBUG
ndpi_print_port_range(pd1->p,pd1->count[0]+pd1->count[1],dbuf,sizeof(dbuf),ndpi_str);
printk("%s: res %d,%d %s\n",__func__,pd1->count[0],pd1->count[1],dbuf);
#endif

ndpi_free(pd);
return pd1;
}

struct ndpi_port_def *ndpi_port_range_update(
		struct ndpi_port_def *pd,
		ndpi_port_range_t *np,
		int proto,int op,
		ndpi_mod_str_t *ndpi_str)
{
ndpi_port_range_t *tp,*tmp;
int i,n,k;
int start,end;
if(!proto) {
	start = 0;
	end   = pd->count[0];
} else {
	start = pd->count[0];
	end   = start + pd->count[1];
}
n = end-start;
DP("%s: %s:%d-%d:%d %d %d %d %s\n",
	np->l4_proto ? (np->l4_proto == 1 ? "tcp":"any"):"udp",
	np->start,np->end,np->proto,start,end,proto,
	op ? "set":"del");

if(!n) {
	if(!op) return pd;
	return ndpi_port_range_replace(pd,start,end,np,1,proto,ndpi_str); // create 1 range
}

tmp = ndpi_malloc(sizeof(ndpi_port_range_t)*(n+2));
if(!tmp) return pd;
memset((char *)tmp,0,sizeof(ndpi_port_range_t)*(n+2));

i = start;
tp = &pd->p[i];
k = 0;

#ifdef NDPI_IPPORT_DEBUG
#define DD1 printk("%s: k=%d tmp %d-%d i=%d tp %d-%d np %d-%d \n", \
		__func__,k,tmp[k].start,tmp[k].end,i,tp->start,tp->end,np->start,np->end);
#else
#define DD1
#endif
for(;i < end && np->start > tp->end; tp++,i++) {
	DD1
	tmp[k++] = *tp;
}
DD1
if(i < end ) {
	DD1;
	if(np->start > tp->start && np->start < tp->end) {
	    tmp[k] = *tp;
	    tmp[k].end = np->start-1;
	    k++;
	    DP("%s: P0 k %d\n",k);
	}
	tmp[k] = *np;
	DD1;
      v0:
	if(np->end < tp->start) {
	    // insert before old ranges
	    DP("%s: P1\n");
	    goto copy_tail;
	}
	if(np->end < tp->end) {
	    k++;
	    tmp[k] = *tp++; i++;
	    tmp[k].start = np->end+1;
	    DP("%s: P2\n");
	    goto copy_tail;
	}
	if(np->end == tp->end) { // override first old range
	    tp++; i++;
	    DP("%s: P3\n");
	    goto copy_tail;
	}
	// np->end > tp->end
	for(; i < end; i++,tp++) {
	    if(np->end > tp->end) continue;
	    goto v0;
	}
	k++;
	// override all old ranges!
	goto check_eq_proto;
} else {
	// append new range
	tmp[k++] = *np;
	DP("%s: P4 k=%d\n",k);
	goto check_eq_proto; // OK
}

copy_tail:
    k++;
    for(; i < end; i++,tp++) tmp[k++] = *tp;

// k - new length of range
check_eq_proto:

#ifdef NDPI_IPPORT_DEBUG
{
char dbuf[128];
ndpi_print_port_range(tmp,k,dbuf,sizeof(dbuf),ndpi_str);
printk("%s: k=%d: %s\n",__func__,k,dbuf);
}
#endif

if(k <= 1) {
    if(!op) k = 0;
} else {
    int l;
    if(!op) {
	for(l = 0 , i = 0; i < k; i++) {
	    if(tmp[i].start == np->start &&
		tmp[i].end   == np->end &&
		tmp[i].proto == np->proto) {
		continue;	
	    }
	    if(i != l) tmp[l++] = tmp[i];
	}
	k = l;
    }

    for(l = 0; l < k-1; ) {
	i = l+1;
	DP("%s: l=%d %d-%d.%d i=%d %d-%d.%d n=%d\n",
		l,tmp[l].start,tmp[l].end,tmp[l].proto,
		i,tmp[i].start,tmp[i].end,tmp[i].proto,n);

	if(tmp[l].proto != tmp[i].proto ||
	   tmp[l].end+1 != tmp[i].start) {
	    l++;
	    continue;
	}
	tmp[l].end = tmp[i].end;
	for(;i < k-1; i++) tmp[i] = tmp[i+1];
	k--;
    }
    k = l+1;
}
DP("%s: k %d\n",k);

pd = ndpi_port_range_replace(pd,start,end,tmp,k,proto,ndpi_str);
ndpi_free(tmp);
return pd;
}

void *ndpi_port_add_one_range(void *data, ndpi_port_range_t *np,int op,
		ndpi_mod_str_t *ndpi_str)
{
struct ndpi_port_def *pd = data;
int i;
if(!pd) {
	if(!op) return data;

	i = np->l4_proto != 2 ? 1:2;
	pd = ndpi_malloc(sizeof(struct ndpi_port_def) + sizeof(ndpi_port_range_t)*i);
	if(!pd) return pd;

	pd->p[0] = *np;
	pd->count[0] = np->l4_proto != 1;
	pd->count[1] = np->l4_proto > 0;
	if(np->l4_proto == 2) {
		pd->p[1] = *np;
		pd->p[0].l4_proto = 0;
		pd->p[1].l4_proto = 1;
	}
	return pd;
}
if(np->l4_proto != 1) { // udp or any
	pd = ndpi_port_range_update(pd,np,0,op,ndpi_str);
}
if(np->l4_proto > 0 ) { // tcp or any
	pd = ndpi_port_range_update(pd,np,1,op,ndpi_str);
}
if(pd && (pd->count[0] + pd->count[1]) == 0) {
	ndpi_free(pd);
	pd = NULL;
}
return pd;
}

static int parse_ndpi_ipdef_cmd(struct ndpi_net *n, int f_op, prefix_t *prefix, char *arg) {
char *d1,*d2;
int f_op2=0;
int ret = 0;

ndpi_port_range_t np = { .start=0, .end=0, .l4_proto = 2, .proto = NDPI_PROTOCOL_UNKNOWN };
patricia_node_t *node;
patricia_tree_t *pt;

if(*arg == '-') {
	f_op2=1;
	arg++;
}
d1 = strchr(arg,':');
if(d1) *d1++='\0';
d2 = d1 ? strchr(d1,':'):NULL;
if(d2) *d2++='\0';
DP("%s(op=%d, %s %s %s)\n",f_op,arg,d1 ? d1: "(NULL)",d2 ? d2:"(NULL)");
if(d1) {
    if(d2) { // full spec
	if(parse_l4_proto(arg,&np)) return 1;
	if(parse_port_range(d1,&np)) return 1;
	if(parse_n_proto(d2,&np,n->ndpi_struct)) return 1;
	DP("%s:3 proto %d start %d end %d l4 %d\n",
			np.proto, np.start, np.end, np.l4_proto);
    } else {
	// port:protocol
	if(parse_port_range(arg,&np)) {
		//l4:proto
		if(parse_l4_proto(arg,&np)) return 1;
		np.end=65535;
	}
	if(parse_n_proto(d1,&np,n->ndpi_struct)) return 1;
	DP("%s:2 proto %d start %d end %d l4 %d\n",
			np.proto, np.start, np.end, np.l4_proto);
    }
} else {
	if(parse_n_proto(arg,&np,n->ndpi_struct)) return 1;
	DP("%s:1 proto %d\n",np.proto);
}

spin_lock_bh (&n->ipq_lock);

do {
pt = n->ndpi_struct->protocols_ptree;
node = ndpi_patricia_search_exact(pt,prefix);
DP(node ? "%s: Found node\n":"%s: Node not found\n");
if(f_op || f_op2) { // delete
	if(!node) break;
	// -xxxx any
	if(np.proto >= NDPI_NUM_BITS && 
		np.l4_proto == 2 && !np.start && !np.end) {
		ndpi_patricia_remove(pt,node);
		break;
	}
	if(!np.start && !np.end) {
	    // -xxxx proto
	    if(node->value.user_value == np.proto) {
		if(!node->data) {
		    ndpi_patricia_remove(pt,node);
		} else {
		  node->value.user_value = 0;
		}
		break;
	    }
	}
	node->data = ndpi_port_add_one_range(node->data,&np,0,n->ndpi_struct);
	break;
}
// add or change
if(!node) {
	node = ndpi_patricia_lookup(pt, prefix);
	if(!node) {
		ret = 1; break;
	}
}

if(np.proto == NDPI_PROTOCOL_UNKNOWN || 
   np.proto >= NDPI_NUM_BITS) {
	ret = 1; break;
}

if(!np.start && !np.end) {
	if(np.l4_proto == 2) {
	    // any:proto
	    node->value.user_value = np.proto;
	    break;
	}
	// (tcp|udp):any:proto
	np.start=1;
	np.end = 65535;
}
node->data = ndpi_port_add_one_range(node->data,&np,1,n->ndpi_struct);
} while (0);

spin_unlock_bh (&n->ipq_lock);

return ret;
}

#define SKIP_SPACE_C while(!!(c = *cmd) && (c == ' ' || c == '\t')) cmd++
#define SKIP_NONSPACE_C while(!!(c = *cmd) && (c != ' ') && (c != '\t')) cmd++

/*
 * [-]prefix ([[(tcp|udp|any):]port[-port]:]protocol)+
 */

static int parse_ndpi_ipdef(struct ndpi_net *n,char *cmd) {
int f_op = 0; // 1 if delete
char *addr,c;
prefix_t *prefix;
int res = 0;

SKIP_SPACE_C;
if(*cmd == '#') return 0;
if(*cmd == '-') {
    f_op = 1; cmd++;
} else 
    if(*cmd == '+') cmd++;

addr = cmd;
SKIP_NONSPACE_C;
if (*cmd) *cmd++ = 0;
SKIP_SPACE_C;
if(!*addr) return 1;
DP("%s: prefix %s\n",addr);
prefix = ndpi_ascii2prefix(AF_INET,addr);
if(!prefix) {
	DP("%s: bad IP '%s'\n",addr);
	return 1;
}

while(*cmd && !res) {
	char *t = cmd;
	SKIP_NONSPACE_C;
	if(*cmd) *cmd++ = 0;
	SKIP_SPACE_C;
	if(parse_ndpi_ipdef_cmd(n,f_op,prefix,t)) res=1;
}
ndpi_Deref_Prefix (prefix);
return res;
}

static int
generic_proc_close(struct ndpi_net *n,
		     int (*parse_line)(struct ndpi_net *n,char *cmd),
		     write_buf_id_t id)
{
	struct write_proc_cmd *w_buf;
	int ret = 0;

	spin_lock(&n->w_buff_lock);
	w_buf = n->w_buff[id];
	n->w_buff[id] = NULL;
	spin_unlock(&n->w_buff_lock);

	if(w_buf) {
		if(w_buf->cpos ) {
			if(ndpi_log_debug > 1)
			    pr_info("%s: cmd %d:%s\n",__func__,
					    w_buf->cpos,&w_buf->cmd[0]);
			ret = (parse_line)(n,&w_buf->cmd[0]);
		}
		kfree(w_buf);
	}
	return ret;
}

static struct write_proc_cmd * alloc_proc_wbuf(struct ndpi_net *n,
					write_buf_id_t id,size_t cmd_len_max) {
	struct write_proc_cmd *ret;
	spin_lock(&n->w_buff_lock);
	ret = n->w_buff[id];
	if(!ret) {
		ret = kmalloc(sizeof(struct write_proc_cmd) + cmd_len_max + 1,
				GFP_KERNEL);
		if(ret) {
			ret->max = cmd_len_max;
			ret->cpos = 0;
			memset(&ret->cmd[0],0,cmd_len_max);
			n->w_buff[id] = ret;
		}
	}
	spin_unlock(&n->w_buff_lock);
	return ret;
}


static ssize_t
generic_proc_write(struct file *file, const char __user *buffer,
                     size_t length, loff_t *loff, 
		     int (*parse_line)(struct ndpi_net *n,char *cmd),
		     size_t cmd_size,write_buf_id_t id)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
	char c,buf[1024+1];
	struct write_proc_cmd *w_buf;
	int pos,i,l,r,skip;

	if (length <= 0) return length;
	pos = 0;

	w_buf =  alloc_proc_wbuf(n,id,cmd_size);
	if(!w_buf) return -ENOBUFS;
	skip = w_buf->cpos == w_buf->max - 1;

	while(pos < length) {
		l = min(length,sizeof(buf)-1);
	
		memset(buf,0,sizeof(buf));
		if (!(access_ok(VERIFY_READ, buffer+pos, l) && 
			!__copy_from_user(&buf[0], buffer+pos, l)))
			        return -EFAULT;
		for(i = 0; i < l; i++) {
			c = buf[i];
			if(c == '\n' || !c) {
				if(w_buf->cpos) {
					if(ndpi_log_debug > 1)
					    pr_info("%s: cmd %d:%s\n", __func__,
							    w_buf->cpos,&w_buf->cmd[0]);
					r = (parse_line)(n,&w_buf->cmd[0]);
				}
				skip = 0;
				w_buf->cpos = 0;
				memset(&w_buf->cmd[0],0,cmd_size);
				if(r) return -EINVAL;
			} else {
				if(w_buf->cpos < w_buf->max - 1)
					w_buf->cmd[w_buf->cpos++] = c;
				    else {
					    if(!skip) pr_err("xt_ndpi: Command too long\n");
					    skip = 1;
					}
			}
		}
		pos += l;
        }
        return length;
}

static int n_ipdef_proc_open(struct inode *inode, struct file *file)
{
        return 0;
}

static int n_ipdef_proc_close(struct inode *inode, struct file *file)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
	generic_proc_close(n,parse_ndpi_ipdef,W_BUF_IP);
        return 0;
}

static ssize_t
n_ipdef_proc_write(struct file *file, const char __user *buffer,
                     size_t length, loff_t *loff)
{
	return generic_proc_write(file, buffer, length, loff,
			parse_ndpi_ipdef, 4060 , W_BUF_IP);
}

static ssize_t n_ipdef_proc_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
	patricia_tree_t *pt;
	prefix_t *px;
	patricia_node_t *Xstack[PATRICIA_MAXBITS+1], **Xsp, *node;
	char lbuf[512];
	char ibuf[64];
	int i,l,p;

	i = 0; p = 0;
	pt = n->ndpi_struct->protocols_ptree;

	Xsp = &Xstack[0];
	node = pt->head;
	while (node) {
	    if (node->prefix) {

	      if(i >= *ppos ) {

		l = i ? 0: snprintf(lbuf,sizeof(lbuf),"#ip              proto\n");
		
		px = node->prefix;
		{
		int k;
		inet_ntop(px->family,(void *)&px->add,ibuf,sizeof(ibuf)-1);
		k = strlen(ibuf);
		if((px->family == AF_INET  && px->bitlen < 32 ) ||
		   (px->family == AF_INET6 && px->bitlen < 128 ))
			snprintf(&ibuf[k],sizeof(ibuf)-k,"/%d",px->bitlen);
		}
		if(node->value.user_value != NDPI_PROTOCOL_UNKNOWN)
		    l += snprintf(&lbuf[l],sizeof(lbuf)-l,"%-16s %s\n",ibuf,
			node->value.user_value >= NDPI_NUM_BITS ?
				"unknown":ndpi_get_proto_by_id(n->ndpi_struct,node->value.user_value));
		if(node->data) {
			struct ndpi_port_def *pd = node->data;
			ndpi_port_range_t *pt = pd->p;
			if(pd->count[0]+pd->count[1] > 0) {
			l += snprintf(&lbuf[l],sizeof(lbuf)-l,"%-16s ",ibuf);
			l += ndpi_print_port_range(pt,pd->count[0]+pd->count[1],
					&lbuf[l],sizeof(lbuf)-l,n->ndpi_struct);
			l += snprintf(&lbuf[l],sizeof(lbuf)-l,"\n");
			}
		}
		if(count < l) break;
		
		if (!(access_ok(VERIFY_WRITE, buf+p, l) &&
				!__copy_to_user(buf+p, lbuf, l))) return -EFAULT;
		p += l;
		count -= l;
		(*ppos)++;
	      }
	      i++;
	    }
	    if (node->l) {
		if (node->r) {
		    *Xsp++ = node->r;
		}
		node = node->l;
		continue;
	    }
	    if (node->r) {
		node = node->r;
		continue;
	    }
	    node = Xsp != Xstack ? *(--Xsp): NULL;
	}
	return p;
}


/********************* ndpi proto *********************************/

static int parse_ndpi_proto(struct ndpi_net *n,char *cmd) {
	char *v,*m,*hid;
	v = cmd;
	if(!*v) return 0;
/*
 * hexID hexmark/mask name
 * hexID debug 0..3
 * hexID disable
 * hexID enable (ToDo)
 * add_custom name
 */
	while(*v && (*v == ' ' || *v == '\t')) v++;
	if(*v == '#') return 0;
	// first word (hid)
	hid = v;
	while(*v && !(*v == ' ' || *v == '\t')) v++; // first word
	if(*v) *v++ = '\0';
	while(*v && (*v == ' ' || *v == '\t')) v++; // space
	// second word (v)
	for(m = v; *m ; m++) {
		if(*m != '#') continue;
		// remove comment 
		*m = '\0';
		// remove spaces before comment
		while (m--, m > v && (*m == ' ' || *m == '\t')) {
			*m = '\0';
		}
		break;
	}
	for(m = v; *m && *m != '/';m++);

	if(*m) {
		char *x;
		*m++ = '\0';
		x = m;
		while(*x && !(*x == ' ' || *x == '\t')) x++;
		if(*x) *x++ = '\0';
	}
	if(*v) {
		u_int32_t mark,mask;

		int id=-1;
		int i,any,all,ok;

		any = !strcmp(hid,"any");
		all = !strcmp(hid,"all");
		mark = 0;
		mask = 0xffff;

		if(!strcmp(hid,"add_custom")) {
			u_int16_t e_proto;
			char *e;
			// v -> name of custom protocol
			for(e = v; *e && *e != ' ' && *e != '\t'; e++) { // first space or eol
				if(*e < ' ' || strchr("/&^:;\\\"'",*e)) {
					if(*e < ' ')
					    pr_err("NDPI: can't use '\\0x%x' in protocol name\n",*e & 0xff);
					  else
					    pr_err("NDPI: can't use '%c' in protocol name\n",*e & 0xff);
					return 1;
				}
			}
			if(*e) *e = '\0';
			e_proto = ndpi_get_proto_by_name(n->ndpi_struct,v);
			if(e_proto != NDPI_PROTOCOL_UNKNOWN) {
				pr_err("NDPI: '%s' exists\n",v);
				return 0;
			}
			if(atomic_read(&n->protocols_cnt[0])) {
				pr_err("NDPI: iptables in use. can't create custom protocol! See README\n");
				return 1;
			}
			v--;
			*v = '@';
			id = ndpi_handle_rule(n->ndpi_struct, v , 1);
			if(id < 0) {
				pr_err("NDPI: ndpi_handle_rule error %d\n",id);
				return 1;
			}
			e_proto = ndpi_get_proto_by_name(n->ndpi_struct,v+1);
			if(ndpi_log_debug > 1)
				pr_info("NDPI: add custom protocol %x\n",e_proto);
			n->mark[e_proto].mark = e_proto;
			n->mark[e_proto].mask = 0x1ff;
			return 0;
		}

		if(kstrtoint(hid,16,&id)) {
			id = -1;
			id = ndpi_get_proto_by_name(n->ndpi_struct,hid);
			if(id == NDPI_PROTOCOL_UNKNOWN && !(all || any)) {
				pr_err("NDPI: '%s' unknown protocol or not hexID\n",hid);
				return 1;
			}
		} else {
			if(id < 0 || id >= NDPI_NUM_BITS) {
				pr_err("NDPI: bad id %d\n",id);
				id = -1;
			}
		}
		if(!strncmp(v,"debug",5)) {
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
			u_int8_t dbg_lvl = 0;
			m = v+5;
			if(*m && *m != ' ' && *m != '\t') {
				pr_err("NDPI: invalid debug settings\n");
				return 1;
			}
			while(*m && (*m == ' ' || *m == '\t')) m++; // space
			if(*m < '0' || *m > '4') {
				pr_err("NDPI: debug level must be 0..4\n");
				return 1;
			}

			dbg_lvl = *m - '0';

			if(all || any) {
				for(i=0; i < NDPI_NUM_BITS; i++)
						n->debug_level[i] = dbg_lvl;
				set_debug_trace(n);
				return 0;
			}
			if(id >= 0 && id < NDPI_NUM_BITS) {
				n->debug_level[id] = dbg_lvl;
				set_debug_trace(n);
				return 0;
			}
			pr_err("%s:%d BUG! id %s\n",__func__,__LINE__,hid);
			return 1;
#else
			pr_err("NDPI: debug not enabled.\n");
#endif
		}
		/* FIXME enable */
		if(!strncmp(v,"disable",7)) {
			mark = 0;
			mask = 0;
			m = v;
			if(any || all) {
				pr_err("NDPI: can't disable all\n");
				return 1;
			}
		} else {
		    /* set mark/mask */
		    if(kstrtou32(v,16,&mark)) {
			pr_err("NDPI: bad mark '%s'\n",v);
			return 1;
		    }
		    if(*m) {
			if(kstrtou32(m,16,&mask)) {
				pr_err("NDPI: bad mask '%s'\n",m);
				return 1;
			}
		    }
		}
//		printk("NDPI: proto %s id %d mark %x mask %s\n",
//				hid,id,mark,m);
		if(atomic_read(&n->protocols_cnt[0]) &&
			!mark && !mask) {
			pr_err("NDPI: iptables in use! Can't disable protocol\n");
			return 1;
		}
		if(id != -1) {
			n->mark[id].mark = mark;
			if(*m) 	n->mark[id].mask = mask;
			return 0;
		}
		/* all or any */
		for(i=0; i < NDPI_NUM_BITS; i++) {
			const char *t_proto = ndpi_get_proto_by_id(n->ndpi_struct,i);
			if(!t_proto) continue;
			if(any && !i) continue;

			n->mark[i].mark = mark;
			if(*m) 	n->mark[i].mask = mask;
			ok++;
//			printk("Proto %s id %02x mark %08x/%08x\n",
//					cmd,i,n->mark[i].mark,n->mark[i].mask);
		}
		return 0;
	}
	if(!strcmp(hid,"init")) {
		int i;
		for(i=0; i < NDPI_NUM_BITS; i++) {
			n->mark[i].mark = i;
			n->mark[i].mask = 0x1ff;
		}
		return 0;
	}
	pr_err("NDPI: bad cmd %s\n",hid);
	return *v ? 0:1;
}

static ssize_t nproto_proc_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
	char lbuf[128];
	char c_buf[12];
	int i,l,p;

	for(i = 0,p = 0; i < NDPI_NUM_BITS; i++) {
		const char *t_proto = ndpi_get_proto_by_id(n->ndpi_struct,i);
		if(!t_proto) {
			snprintf(c_buf,sizeof(c_buf)-1,"custom%d",i);
			t_proto = c_buf;
		}

		if(i < *ppos ) continue;
		l = i ? 0: snprintf(lbuf,sizeof(lbuf),
				"#id     mark ~mask     name   # count #version %s\n",
				NDPI_GIT_RELEASE);
		if(!n->mark[i].mark && !n->mark[i].mask)
		    l += snprintf(&lbuf[l],sizeof(lbuf)-l,"%02x  %17s %-16s # %d\n",
				i,"disabled",t_proto ? t_proto:"bad",
				atomic_read(&n->protocols_cnt[i]));
		else
		    l += snprintf(&lbuf[l],sizeof(lbuf)-l,"%02x  %8x/%08x %-16s # %d debug=%d\n",
				i,n->mark[i].mark,n->mark[i].mask,t_proto ? t_proto :"bad",
				atomic_read(&n->protocols_cnt[i]),
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
					n->debug_level[i]
#else
					0
#endif
					);

		if(count < l) break;

		if (!(access_ok(VERIFY_WRITE, buf+p, l) &&
				!__copy_to_user(buf+p, lbuf, l))) return -EFAULT;
		p += l;
		count -= l;
		(*ppos)++;
	}
	return p;
}

static int nproto_proc_close(struct inode *inode, struct file *file)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
	generic_proc_close(n,parse_ndpi_proto,W_BUF_PROTO);
        return 0;
}

static ssize_t
nproto_proc_write(struct file *file, const char __user *buffer,
                     size_t length, loff_t *loff)
{
	return generic_proc_write(file, buffer, length, loff,
			parse_ndpi_proto, 256, W_BUF_PROTO);
}

/********************* ndpi host_def  *********************************/

static const char *__acerr2txt[] = {
    [ACERR_SUCCESS] = "OK", /* No error occurred */
    [ACERR_DUPLICATE_PATTERN] = "ERR:DUP", /* Duplicate patterns */
    [ACERR_LONG_PATTERN] = "ERR:LONG", /* Pattern length is longer than AC_PATTRN_MAX_LENGTH */
    [ACERR_ZERO_PATTERN] = "ERR:EMPTY" , /* Empty pattern (zero length) */
    [ACERR_AUTOMATA_CLOSED] = "ERR:CLOSED", /* Automata is closed. */
    [ACERR_ERROR] = "ERROR" /* common error */
};

static const char *acerr2txt(AC_ERROR_t r) {
	return r >= ACERR_SUCCESS && r <= ACERR_ERROR ? __acerr2txt[r]:"UNKNOWN";
}

static int n_hostdef_proc_open(struct inode *inode, struct file *file)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
	str_collect_t *ph;
	char *host;
	AC_PATTERN_t ac_pattern;
	AC_ERROR_t r;
	int np,nh,ret = 0;

spin_lock(&n->host_lock);
do {
	if(n->hosts_tmp) {
		ret = EBUSY; break;
	}
		
	n->hosts_tmp = str_collect_clone(n->hosts);
	if(!n->hosts_tmp) {
		ret = ENOMEM; break;
	}

	if((file->f_mode & (FMODE_READ|FMODE_WRITE)) == FMODE_READ)
		break;

	if(n->host_ac) {
		ret = EBUSY; break;
	}

	n->host_ac = ndpi_init_automa();

	if(!n->host_ac) {
		str_hosts_done(n->hosts_tmp);
		n->hosts_tmp = NULL;
		ret = ENOMEM; break;
	}
	if(ndpi_log_debug > 1)
		pr_info("host_ac %px new\n",n->host_ac);

	n->host_error = 0;

	for(np = 0; np < NDPI_NUM_BITS; np++) {
		ph = n->hosts_tmp->p[np];
		if(ph) {
			nh = 0;
			for(nh = 0 ; nh < ph->last && ph->s[nh] ;
					nh += (uint8_t)ph->s[nh] + 2) {
				host = &ph->s[nh+1];
				ac_pattern.astring = host;
				ac_pattern.length = strlen(host);
				ac_pattern.rep.number = np;
				r = ac_automata_add(n->host_ac, &ac_pattern);
				if(r != ACERR_SUCCESS) {
					pr_err("%s: host add '%s' : %s : skipped\n",__func__,
							host,acerr2txt(r));
				}
			}
		}
	}
} while(0);

	spin_unlock(&n->host_lock);
	if(ndpi_log_debug > 1)
		pr_info("open: host_ac %px old %px\n",
				(void *)n->host_ac,
				ndpi_automa_host(n->ndpi_struct));
        return ret;
}

static ssize_t n_hostdef_proc_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
	char lbuf[256+32],*host;
	const char *t_proto;
	str_collect_t *ph;
	int i, l=0, p=0, bpos = 0, hl, pl;
	int hdp = 0, hdh = 0;
	loff_t cpos = 0;

	if(ndpi_log_debug > 1)
		pr_info("read: start ppos %lld\n",*ppos);

	while(hdp < NDPI_NUM_BITS ) {
		if(!cpos) {
			strcpy(lbuf,"#Proto:host\n");
			l = strlen(lbuf);
		}

		ph = n->hosts_tmp->p[hdp];
		host = NULL;
		if(ph && ph->last && hdh < ph->last ) {
			t_proto = ndpi_get_proto_by_id(n->ndpi_struct,hdp);
			pl = strlen(t_proto);
			i = 0; p = 0;
			for( ; (uint8_t)ph->s[hdh] ;
					hdh += (uint8_t)ph->s[hdh] + 2) {
				host = &ph->s[hdh+1];
				hl = strlen(host);

				if(i && l - p + hl > 80) break;

				if(hl + 1 + (!i ? pl:0) + l + 5 > sizeof(lbuf)) {
					if(hl + pl + 3 > sizeof(lbuf)) {
						pr_err("ndpi: lbuf too small\n");
						continue;
					}
					if(ndpi_log_debug > 1) 
						pr_info("read:3 lbuff full\n");
					break;
				}
				if(!i) { // start line from protocol name
				    if(p) lbuf[l++] = '\n';
				    strcpy(&lbuf[l],t_proto);
				    l += pl;
				    lbuf[l] = '\0';
				}
				lbuf[l++] = i ? ',':':';
				strcpy(&lbuf[l],host);
				l += hl;
				lbuf[l] = '\0';

				i++;
			}
			lbuf[l++] = '\n'; lbuf[l] = '\0';

			if(hdh == ph->last) // last hostdef for current protocol
				host = NULL;
			if(ndpi_log_debug > 1) 
				pr_info("read:4 lbuf:%d '%s'\n",l,lbuf);

			if(cpos + l < *ppos) {
				cpos += l;
			} else {
				p = 0;
				// ppos: buf + count, cpos: lbuf + l
				if(cpos < *ppos) {
					p = *ppos - cpos;
					l -= p;
				}
				if( l > count) l = count;
				if (!(access_ok(VERIFY_WRITE, buf+bpos, l) &&
					!__copy_to_user(buf+bpos, lbuf+p, l))) return -EFAULT;
				if(ndpi_log_debug > 1) 
					pr_info("read:5 copy bpos %d p %d l %d\n",bpos,p,l);
				(*ppos) += l;
				bpos  += l;
				cpos  += l+p;
				count -= l;
				if(!count) {
					if(ndpi_log_debug > 1) 
						pr_info("read:6 buf full, bpos %d\n",bpos);
					return bpos;
				}
			}
			l = 0;
		}

		if(!host) {
			hdp++;
			hdh = 0;
			continue;
		}
		if(ndpi_log_debug > 1) 
			pr_info("read:7 next\n");
	}
	if(ndpi_log_debug > 1) 
		pr_info("read:8 return bpos %d\n",bpos);
	return bpos;
}

/*
 * Syntax: 
 * reset
 * proto:host[,host...][[ \t;]proto:host[,host...]]
 */

static int parse_ndpi_hostdef(struct ndpi_net *n,char *cmd) {

    AC_PATTERN_t ac_pattern;
    char *pname,*host_match,*nc,*nh,*cstr;
    uint16_t protocol_id;
    AC_ERROR_t r;

    nc = NULL;

    for(; cmd && *cmd ; cmd = nc ) {
	if(*cmd == '#') break;

	while(*cmd && (*cmd == ' ' || *cmd == '\t')) cmd++;
	if(ndpi_log_debug > 1)
		pr_info("%s: %.100s\n",__func__,cmd);
	if(!strcmp(cmd,"reset")) {

		if(ndpi_log_debug > 1)
			pr_info("hostdef: clean host_ac %px\n",n->host_ac);

		ac_automata_clean((AC_AUTOMATA_t*)n->host_ac);

		for(protocol_id = 0; protocol_id < NDPI_NUM_BITS; protocol_id++) {
			if(n->hosts_tmp->p[protocol_id]) {
				kfree(n->hosts_tmp->p[protocol_id]);
				n->hosts_tmp->p[protocol_id] = NULL;
			}
		}
		n->host_error = 0;

		if(ndpi_log_debug > 1)
			pr_info("xt_ndpi: reset hosts\n");
		break;
	}
	pname = cmd;
	host_match = strchr(pname,':');

	if(!*pname || !host_match)
		goto bad_cmd;

	*host_match++ = '\0';

	nc = strchr(host_match,' ');
	if(!nc) nc = strchr(host_match,'\t');
	if(!nc) nc = strchr(host_match,';');

	if(nc) {
		*nc++ = '\0';
		while(*nc && (*nc == ' ' || *nc == '\t' || *nc == ';')) nc++;
	}

	protocol_id =  ndpi_get_proto_by_name(n->ndpi_struct, pname);

	if(protocol_id == NDPI_PROTOCOL_UNKNOWN) {
		pr_err("xt_ndpi: unknown protocol %s\n",pname);
		goto bad_cmd;
	}
	
	if(protocol_id >= NDPI_NUM_BITS) {
		pr_err("xt_ndpi: bad protoId=%u\n", protocol_id);
		goto bad_cmd;
	}

	for(nh = NULL; host_match && *host_match; host_match = nh) {
		size_t sml;
		nh = strchr(host_match,',');
		if(nh) *nh++ = '\0';

		sml = strlen(host_match);
		cstr = str_collect_add(&n->hosts_tmp->p[protocol_id],host_match,sml);
		if(!cstr) {
			pr_err("xt_ndpi: can't alloc memory for '%.60s'\n",host_match);
			goto bad_cmd;
		}

		ac_pattern.astring    = cstr;
		ac_pattern.length     = sml;
		ac_pattern.rep.number = protocol_id;
		r = n->host_ac ? ac_automata_add(n->host_ac, &ac_pattern) : ACERR_ERROR;
		if(r != ACERR_SUCCESS) {
			str_collect_del(n->hosts_tmp->p[protocol_id],host_match,sml);
			if(r != ACERR_DUPLICATE_PATTERN) {
				pr_info("xt_ndpi: add host '%s' proto %s error: %s\n",
						host_match,pname,acerr2txt(r));
				goto bad_cmd;
			}
			if(ac_pattern.rep.number != protocol_id) {
				pr_info("xt_ndpi: Host '%s' proto %s already defined as %s\n",
					host_match,pname,
					ndpi_get_proto_by_id(n->ndpi_struct,
								     ac_pattern.rep.number));
				goto bad_cmd;
			}
		}
	}
	if(ndpi_log_debug > 2 && nc && *nc)
		pr_info("xt_ndpi: next part '%s'\n",nc);
    }
    return 0;

bad_cmd:
    n->host_error++;
    return 1;
}


static int n_hostdef_proc_close(struct inode *inode, struct file *file)
{
        struct ndpi_net *n = PDE_DATA(file_inode(file));
	ndpi_mod_str_t *nstr = n->ndpi_struct;

	generic_proc_close(n,parse_ndpi_hostdef,W_BUF_HOST);

	spin_lock(&n->host_lock);

	if(n->host_ac) {
		if(!n->host_error) {

			ac_automata_finalize((AC_AUTOMATA_t*)n->host_ac);

			spin_lock_bh(&nstr->host_automa_lock);
			XCHGP(nstr->host_automa.ac_automa,n->host_ac);
			spin_unlock_bh(&nstr->host_automa_lock);

			XCHGP(n->hosts,n->hosts_tmp);

		} else {
			pr_err("xt_ndpi: Can't update host_proto with errors\n");
		}

		if(ndpi_log_debug > 1)
			pr_info("close: release host_ac %px\n",n->host_ac);

		ac_automata_release((AC_AUTOMATA_t*)n->host_ac);
		n->host_ac = NULL;
	}
	str_hosts_done(n->hosts_tmp);
	n->hosts_tmp = NULL;

	spin_unlock(&n->host_lock);
        return 0;
}

static ssize_t
n_hostdef_proc_write(struct file *file, const char __user *buffer,
                     size_t length, loff_t *loff)
{
	return generic_proc_write(file, buffer, length, loff,
			parse_ndpi_hostdef, 4060, W_BUF_HOST);
}


static const struct file_operations nproto_proc_fops = {
        .open    = ninfo_proc_open,
        .read    = nproto_proc_read,
        .write   = nproto_proc_write,
	.llseek  = noop_llseek,
	.release = nproto_proc_close
};

static const struct file_operations ninfo_proc_fops = {
        .open    = ninfo_proc_open,
        .read    = ninfo_proc_read,
        .write   = ninfo_proc_write,
	.llseek  = noop_llseek,
};

#ifdef NDPI_DETECTION_SUPPORT_IPV6
static const struct file_operations ninfo6_proc_fops = {
        .open    = ninfo_proc_open,
        .read    = ninfo6_proc_read,
        .write   = ninfo_proc_write,
	.llseek  = noop_llseek,
};
#endif
#ifdef BT_ANNOUNCE
static const struct file_operations nann_proc_fops = {
        .open    = ninfo_proc_open,
        .read    = nann_proc_read,
	.llseek  = noop_llseek,
};
#endif

static const struct file_operations n_ipdef_proc_fops = {
        .open    = n_ipdef_proc_open,
        .read    = n_ipdef_proc_read,
        .write   = n_ipdef_proc_write,
	.llseek  = noop_llseek,
        .release = n_ipdef_proc_close,
};

static const struct file_operations n_hostdef_proc_fops = {
        .open    = n_hostdef_proc_open,
        .read    = n_hostdef_proc_read,
        .write   = n_hostdef_proc_write,
        .llseek  = noop_llseek,
        .release = n_hostdef_proc_close,
};

static void __net_exit ndpi_net_exit(struct net *net)
{
	struct rb_node * next;
	struct osdpi_id_node *id;
	struct ndpi_net *n;

	n = ndpi_pernet(net);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
	del_timer(&n->gc);
#else
	del_timer_sync(&n->gc);
#endif

#ifndef NF_CT_CUSTOM
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
	net->ct.label_words = n->labels_word;
#endif
	net->ct.labels_used--;
#endif

#if   LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
	nf_ct_iterate_cleanup_net(net, __ndpi_free_flow, n, 0 ,0);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 12, 0)
	nf_ct_iterate_cleanup(net, __ndpi_free_flow, n, 0 ,0);
#else /* < 3.12 */
	nf_ct_iterate_cleanup(net, __ndpi_free_flow, n);
#endif
	/* free all objects before destroying caches */
	
	next = rb_first(&n->osdpi_id_root);
	while (next) {
		id = rb_entry(next, struct osdpi_id_node, node);
		next = rb_next(&id->node);
		rb_erase(&id->node, &n->osdpi_id_root);
		kmem_cache_free (osdpi_id_cache, id);
	}
	
	str_hosts_done(n->hosts);
	
	ndpi_exit_detection_module(n->ndpi_struct);

	if(n->pde) {
		if(n->pe_ipdef)
			remove_proc_entry(ipdef_name, n->pde);
		if(n->pe_hostdef)
			remove_proc_entry(hostdef_name, n->pde);
		if(n->pe_info)
			remove_proc_entry(info_name, n->pde);
		if(n->pe_proto)
			remove_proc_entry(proto_name, n->pde);
#ifdef BT_ANNOUNCE
		if(n->pe_ann)
			remove_proc_entry(ann_name, n->pde);
#endif
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		if(n->pe_info6)
			remove_proc_entry(info6_name, n->pde);
#endif
		PROC_REMOVE(n->pde,net);
	}
}

static int __net_init ndpi_net_init(struct net *net)
{
	struct ndpi_net *n;
	int i;

	/* init global detection structure */

	n = ndpi_pernet(net);
	spin_lock_init(&n->id_lock);
	spin_lock_init(&n->ipq_lock);
	spin_lock_init(&n->host_lock);
	spin_lock_init(&n->w_buff_lock);
	n->w_buff[W_BUF_IP] = NULL;
	n->w_buff[W_BUF_HOST] = NULL;
	n->w_buff[W_BUF_PROTO] = NULL;

	n->host_ac = NULL;
	n->hosts = str_hosts_alloc();
	n->hosts_tmp = NULL;
	n->host_error = 0;

	parse_ndpi_proto(n,"init");
       	n->osdpi_id_root = RB_ROOT;

	/* init global detection structure */
	set_ndpi_ticks_per_second(detection_tick_resolution);
	set_ndpi_malloc(malloc_wrapper);
	set_ndpi_free(free_wrapper);
	n->ndpi_struct = ndpi_init_detection_module();
	if (n->ndpi_struct == NULL) {
		pr_err("xt_ndpi: global structure initialization failed.\n");
                return -ENOMEM;
	}
	n->ndpi_struct->direction_detect_disable = 1;
	/* disable all protocols */
	NDPI_BITMASK_RESET(n->protocols_bitmask);
	ndpi_set_protocol_detection_bitmask2(n->ndpi_struct, &n->protocols_bitmask);

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
	pr_info("ndpi_lib_trace %s\n",ndpi_lib_trace ? "Enabled":"Disabled");
	n->ndpi_struct->user_data = n;
	for (i = 0; i < NDPI_NUM_BITS; i++) {
                atomic_set (&n->protocols_cnt[i], 0);
        	n->debug_level[i] = 0;
		if(i <= NDPI_LAST_IMPLEMENTED_PROTOCOL) continue;
		n->mark[i].mark = n->mark[i].mask = 0;
        }
	n->ndpi_struct->ndpi_log_level = ndpi_lib_trace;
	set_ndpi_debug_function(n->ndpi_struct, ndpi_lib_trace ? debug_printf:NULL);
#endif

	if(bt_hash_size > 512) bt_hash_size = 512;
#ifdef BT_ANNOUNCE
	if(bt_log_size > 512) bt_log_size = 512;
	if(bt_log_size < 32 ) bt_log_size = 0;
#else
	bt_log_size = 0;
#endif
	ndpi_bittorrent_init(n->ndpi_struct,bt_hash_size*1024,bt_hash_tmo,bt_log_size);

	n->n_hash = -1;

	/* Create proc files */
	
	n->pde = proc_mkdir(dir_name, net->proc_net);
	if(!n->pde) {
		ndpi_exit_detection_module(n->ndpi_struct);
		pr_err("xt_ndpi: cant create net/%s\n",dir_name);
		return -ENOMEM;
	}
	do {
		ndpi_protocol_match *hm;
		char *cstr;
		int i2;

		n->pe_info = NULL;
		n->pe_proto = NULL;
#ifdef BT_ANNOUNCE
		n->pe_ann = NULL;
#endif
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		n->pe_info6 = NULL;
#endif
		n->pe_ipdef = NULL;
		n->pe_hostdef = NULL;

		n->pe_info = proc_create_data(info_name, S_IRUGO | S_IWUSR,
					 n->pde, &ninfo_proc_fops, n);
		if(!n->pe_info) {
			pr_err("xt_ndpi: cant create net/%s/%s\n",dir_name,info_name);
			break;
		}
		n->pe_proto = proc_create_data(proto_name, S_IRUGO | S_IWUSR,
					 n->pde, &nproto_proc_fops, n);
		if(!n->pe_proto) {
			pr_err("xt_ndpi: cant create net/%s/%s\n",dir_name,proto_name);
			break;
		}
#ifdef BT_ANNOUNCE
		n->pe_ann = proc_create_data(ann_name, S_IRUGO,
					 n->pde, &nann_proc_fops, n);
		if(!n->pe_ann) {
			pr_err("xt_ndpi: cant create net/%s/%s\n",dir_name,ann_name);
			break;
		}

#endif
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		n->pe_info6 = proc_create_data(info6_name, S_IRUGO | S_IWUSR,
					 n->pde, &ninfo6_proc_fops, n);
		if(!n->pe_info6) {
			pr_err("xt_ndpi: cant create net/%s/%s\n",dir_name,info6_name);
			break;
		}

#endif
		n->pe_ipdef = proc_create_data(ipdef_name, S_IRUGO | S_IWUSR,
					 n->pde, &n_ipdef_proc_fops, n);
		if(!n->pe_ipdef) {
			pr_err("xt_ndpi: cant create net/%s/%s\n",dir_name,ipdef_name);
			break;
		}

		n->pe_hostdef = proc_create_data(hostdef_name, S_IRUGO | S_IWUSR,
					 n->pde, &n_hostdef_proc_fops, n);
		if(!n->pe_hostdef) {
			pr_err("xt_ndpi: cant create net/%s/%s\n",dir_name,hostdef_name);
			break;
		}
		n->host_ac = ndpi_init_automa();
		if(!n->host_ac) {
			pr_err("xt_ndpi: cant alloc host_ac\n");
			break;
		}
		for(hm = host_match; hm->string_to_match ; hm++) {
			size_t sml;
			i = hm->protocol_id;
			if(i >= NDPI_NUM_BITS) {
				pr_err("xt_ndpi: bad proto num %d \n",i);
				continue;
			}
			sml = strlen(hm->string_to_match);
			i2 = ndpi_match_string_subprotocol(n->ndpi_struct,
								hm->string_to_match,sml,1);
			if(i2 == NDPI_PROTOCOL_UNKNOWN || i != i2) {
				pr_err("xt_ndpi: Warning! Hostdef '%s' %s! Skipping.\n",
						hm->string_to_match,
						i != i2 ? "missmatch":"unknown");
				continue;
			}
			if(str_collect_look(n->hosts->p[i],hm->string_to_match,sml) >= 0) {
				pr_err("xt_ndpi: Warning! Hostdef '%s' duplicated! Skipping.\n",
						hm->string_to_match);
				continue;
			}
			cstr = str_collect_add(&n->hosts->p[i],hm->string_to_match,sml);
			if(!cstr) {
				hm = NULL; // error
				break;
			}
			{
				AC_ERROR_t r;
				AC_PATTERN_t ac_pattern;
				ac_pattern.astring    = cstr;
				ac_pattern.length     = sml;
				ac_pattern.rep.number = i;
				r = ac_automata_add(n->host_ac, &ac_pattern);
				if(r != ACERR_SUCCESS) {
					str_collect_del(n->hosts_tmp->p[i],cstr,sml);
					if(r != ACERR_DUPLICATE_PATTERN) {
						pr_info("xt_ndpi: add host '%s' proto %x error: %s\n",
							hm->string_to_match,i,acerr2txt(r));
						hm = NULL; // error
						break;
					}
					if(ac_pattern.rep.number != i) {
						pr_info("xt_ndpi: Host '%s' proto %x already defined as %s\n",
							hm->string_to_match,i, 
							ndpi_get_proto_by_id(n->ndpi_struct,
								     ac_pattern.rep.number));
					}
				}
			}
		}
		if(hm) {
			XCHGP(n->ndpi_struct->host_automa.ac_automa,n->host_ac);
			ac_automata_release(n->host_ac);
			n->host_ac = NULL;
		} else break;

		if(bt_hash_size) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
			init_timer(&n->gc);
			n->gc.data = (unsigned long)n;
			n->gc.function = bt_port_gc;
			n->gc.expires = jiffies + HZ/2;
			add_timer(&n->gc);
#else
			timer_setup(&n->gc, bt_port_gc, 0);
			mod_timer(&n->gc, jiffies + HZ/2);
#endif
		}
#ifndef NF_CT_CUSTOM
		/* hack!!! */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
		n->labels_word = ACCESS_ONCE(net->ct.label_words);
		net->ct.label_words = 2;
#endif
		net->ct.labels_used++;
#endif
		/* All success! */
		return 0;
	} while(0);

/* rollback procfs on error */
	str_hosts_done(n->hosts);

	if(n->pe_hostdef)
		remove_proc_entry(hostdef_name,n->pde);
	if(n->pe_ipdef)
		remove_proc_entry(ipdef_name,n->pde);
#ifdef NDPI_DETECTION_SUPPORT_IPV6
	if(n->pe_info6)
		remove_proc_entry(proto_name, n->pde);
#endif
#ifdef BT_ANNOUNCE
	if(n->pe_ann)
		remove_proc_entry(ann_name, n->pde);
#endif
	if(n->pe_proto)
		remove_proc_entry(proto_name,n->pde);
	if(n->pe_info)
		remove_proc_entry(info_name,n->pde);

	PROC_REMOVE(n->pde,net);
	ndpi_exit_detection_module(n->ndpi_struct);

	return -ENOMEM;
}

#ifndef NF_CT_CUSTOM
static void replace_nf_destroy(void)
{
	void (*destroy)(struct nf_conntrack *);
	rcu_read_lock();
	destroy = rcu_dereference(nf_ct_destroy);
	BUG_ON(destroy == NULL);
	rcu_assign_pointer(ndpi_nf_ct_destroy,destroy);
        RCU_INIT_POINTER(nf_ct_destroy, ndpi_destroy_conntrack);
	rcu_read_unlock();
}

static void restore_nf_destroy(void)
{
	void (*destroy)(struct nf_conntrack *);
	rcu_read_lock();
	destroy = rcu_dereference(nf_ct_destroy);
	BUG_ON(destroy != ndpi_destroy_conntrack);
	destroy = rcu_dereference(ndpi_nf_ct_destroy);
	BUG_ON(destroy == NULL);
	rcu_assign_pointer(nf_ct_destroy,destroy);
	rcu_read_unlock();
}
#else
static struct nf_ct_ext_type ndpi_extend = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,8,0)
       .seq_print = seq_print_ndpi,
#endif
       .destroy   = nf_ndpi_free_flow,
       .len    = sizeof(struct nf_ct_ext_ndpi),
       .align  = __alignof__(uint32_t),
};
#endif

static struct pernet_operations ndpi_net_ops = {
        .init   = ndpi_net_init,
        .exit   = ndpi_net_exit,
        .id     = &ndpi_net_id,
        .size   = sizeof(struct ndpi_net),
};

static int __init ndpi_mt_init(void)
{
        int ret;

	ndpi_size_id_struct = sizeof(struct osdpi_id_node);
	ndpi_size_flow_struct = ndpi_detection_get_sizeof_ndpi_flow_struct();
	detection_tick_resolution = HZ;

	if(request_module("nf_conntrack") < 0) {
		pr_err("xt_ndpi: nf_conntrack required!\n");
		return -EOPNOTSUPP;
	}
	if(request_module("ip_tables") < 0) {
		pr_err("xt_ndpi: ip_tables required!\n");
		return -EOPNOTSUPP;
	}
#ifdef NDPI_DETECTION_SUPPORT_IPV6
	if(request_module("ip6_tables") < 0) {
		pr_err("xt_ndpi: ip6_tables required!\n");
		return -EOPNOTSUPP;
	}
#endif
#ifdef NF_CT_CUSTOM
	ret = nf_ct_extend_custom_register(&ndpi_extend,0x4e445049); /* "NDPI" in hex */
	if(ret < 0) {
		pr_err("xt_ndpi: can't nf_ct_extend_register.\n");
		return -EBUSY;
	}
	nf_ct_ext_id_ndpi = ndpi_extend.id;
#else
	nf_ct_ext_id_ndpi = NF_CT_EXT_LABELS;
#endif

	ret = register_pernet_subsys(&ndpi_net_ops);
	if (ret < 0) {
		pr_err("xt_ndpi: can't register_pernet_subsys.\n");
		goto unreg_ext;
	}

        ret = xt_register_match(&ndpi_mt_reg);
        if (ret) {
                pr_err("xt_ndpi: error registering ndpi match.\n");
		goto unreg_pernet;
        }

        ret = xt_register_target(&ndpi_tg_reg);
        if (ret) {
                pr_err("xt_ndpi: error registering ndpi match.\n");
		goto unreg_match;
        }

	ret = -ENOMEM;

        osdpi_flow_cache = kmem_cache_create("ndpi_flows", ndpi_size_flow_struct,
                                             0, 0, NULL);
        if (!osdpi_flow_cache) {
                pr_err("xt_ndpi: error creating flow cache.\n");
		goto unreg_target;
        }
        
        osdpi_id_cache = kmem_cache_create("ndpi_ids",
                                           ndpi_size_id_struct,
                                           0, 0, NULL);
        if (!osdpi_id_cache) {
		pr_err("xt_ndpi: error creating id cache.\n");
		goto free_flow;
	}

	ndpi_size_hash_ip4p_node=                sizeof(struct hash_ip4p_node)
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		                                 +12
#endif
	;

        bt_port_cache = kmem_cache_create("ndpi_btport",
				ndpi_size_hash_ip4p_node, 0, 0, NULL);
        if (!bt_port_cache) {
		pr_err("xt_ndpi: error creating port cache.\n");
		goto free_id;
	}
	if(bt_hash_size && bt_hash_size > 512) bt_hash_size = 512;
	if(!bt_hash_tmo || bt_hash_tmo < 900) bt_hash_tmo = 900;
	if( bt_hash_tmo > 3600) bt_hash_tmo = 3600;

#ifndef NF_CT_CUSTOM
	replace_nf_destroy();
#endif
	pr_info("xt_ndpi v1.2 ndpi %s"
#ifdef NDPI_DETECTION_SUPPORT_IPV6
		" IPv6=YES"
#else
		" IPv6=no"
#endif
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
		" debug_message=YES"
#else
		" debug_message=no"
#endif
		"\n BT: hash_size %luk, hash_expiation %ld sec, log_size %ldkb\n"
		" sizeof hash_ip4p_node=%lu id_struct=%lu PATRICIA_MAXBITS=%zu\n"
		" flow_struct=%lu packet_struct=%zu\n"
		"   flow_tcp_struct=%zu flow_udp_struct=%zu int_one_line_struct=%zu\n"
		" ndpi_ip_addr_t=%zu ndpi_protocol=%zu nf_ct_ext_ndpi=%zu\n"
		" spinlock_t=%zu\n"
#ifndef NF_CT_CUSTOM
		" NF_LABEL_ID %d\n",
#else
		" NF_EXT_ID %d\n",
#endif
		NDPI_GIT_RELEASE,
		bt_hash_size, bt_hash_size ? bt_hash_tmo : 0, bt_log_size, 
		ndpi_size_hash_ip4p_node, ndpi_size_id_struct, (size_t)PATRICIA_MAXBITS,
		ndpi_size_flow_struct,
		sizeof(struct ndpi_packet_struct),
		sizeof(struct ndpi_flow_tcp_struct),
		sizeof(struct ndpi_flow_udp_struct),
		sizeof(struct ndpi_int_one_line_struct),
		sizeof(ndpi_ip_addr_t),
		sizeof(ndpi_protocol),
		sizeof(struct nf_ct_ext_ndpi),
		sizeof(spinlock_t),
		nf_ct_ext_id_ndpi);
	pr_info("xt_ndpi MAX_PROTOCOLS %d LAST_PROTOCOL %d\n",
		NDPI_NUM_BITS,
		NDPI_LAST_IMPLEMENTED_PROTOCOL);


	return 0;

free_id:
       	kmem_cache_destroy (osdpi_id_cache);
free_flow:
       	kmem_cache_destroy (osdpi_flow_cache);
unreg_target:
	xt_unregister_target(&ndpi_tg_reg);
unreg_match:
	xt_unregister_match(&ndpi_mt_reg);
unreg_pernet:
	unregister_pernet_subsys(&ndpi_net_ops);
unreg_ext:
#ifdef NF_CT_CUSTOM
	nf_ct_extend_unregister(&ndpi_extend);
#endif
       	return ret;
}


static void __exit ndpi_mt_exit(void)
{
	pr_info("xt_ndpi 1.2 unload.\n");

        kmem_cache_destroy (bt_port_cache);
        kmem_cache_destroy (osdpi_id_cache);
        kmem_cache_destroy (osdpi_flow_cache);
	xt_unregister_target(&ndpi_tg_reg);
	xt_unregister_match(&ndpi_mt_reg);
	unregister_pernet_subsys(&ndpi_net_ops);
#ifdef NF_CT_CUSTOM
	nf_ct_extend_unregister(&ndpi_extend);
#else
	restore_nf_destroy();
#endif
}


module_init(ndpi_mt_init);
module_exit(ndpi_mt_exit);
