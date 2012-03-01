/*	$OpenBSD: pf_ioctl.c,v 1.213 2009/02/15 21:46:12 mbalmer Exp $ */

/*
 * Copyright (c) 2001 Daniel Hartmeier
 * Copyright (c) 2002,2003 Henning Brauer
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    - Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    - Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Effort sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F30602-01-2-0537.
 *
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_bpf.h"
#include "opt_pf.h"

#ifdef DEV_PFLOG
#define		NPFLOG		DEV_PFLOG
#else
#define		NPFLOG		0
#endif


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/filio.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/kernel.h>
#include <sys/time.h>
#include <sys/ucred.h>
#include <sys/jail.h>
#include <sys/module.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/malloc.h>
#include <sys/kthread.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/vnet.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_icmp.h>

#include <sys/md5.h>
#include <net/pfvar.h>

#include <net/if_pfsync.h>

#if NPFLOG > 0
#include <net/if_pflog.h>
#endif /* NPFLOG > 0 */

#ifdef INET6
#include <netinet/ip6.h>
#include <netinet/in_pcb.h>
#endif /* INET6 */

#ifdef ALTQ
#include <altq/altq.h>
#endif

#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <net/pfil.h>

static void		 cleanup_pf_zone(void);
int			 pfattach(void);
static struct pf_pool	*pf_get_pool(char *, u_int32_t, u_int8_t, u_int32_t,
			    u_int8_t, u_int8_t, u_int8_t);

static void		 pf_mv_pool(struct pf_palist *, struct pf_palist *);
static void		 pf_empty_pool(struct pf_palist *);
static int		 pfioctl(struct cdev *, u_long, caddr_t, int,
			    struct thread *);
#ifdef ALTQ
static int		 pf_begin_altq(u_int32_t *);
static int		 pf_rollback_altq(u_int32_t);
static int		 pf_commit_altq(u_int32_t);
static int		 pf_enable_altq(struct pf_altq *);
static int		 pf_disable_altq(struct pf_altq *);
static u_int32_t	 pf_qname2qid(char *);
static void		 pf_qid_unref(u_int32_t);
#endif /* ALTQ */
static int		 pf_begin_rules(u_int32_t *, int, const char *);
static int		 pf_rollback_rules(u_int32_t, int, char *);
static int		 pf_setup_pfsync_matching(struct pf_ruleset *);
static void		 pf_hash_rule(MD5_CTX *, struct pf_rule *);
static void		 pf_hash_rule_addr(MD5_CTX *, struct pf_rule_addr *);
static int		 pf_commit_rules(u_int32_t, int, char *);
static int		 pf_addr_setup(struct pf_ruleset *,
			    struct pf_addr_wrap *, sa_family_t);
static void		 pf_addr_copyout(struct pf_addr_wrap *);
static void		 pf_pkt_addr_changed(struct mbuf *);

#define	TAGID_MAX	 50000

VNET_DEFINE(struct pf_rule,	 pf_default_rule);
VNET_DEFINE(struct sx,		 pf_consistency_lock);

#ifdef ALTQ
static VNET_DEFINE(int,		pf_altq_running);
#define	V_pf_altq_running	VNET(pf_altq_running)
#endif

TAILQ_HEAD(pf_tags, pf_tagname);

#define	V_pf_tags		VNET(pf_tags)
VNET_DEFINE(struct pf_tags, pf_tags);
#define	V_pf_qids		VNET(pf_qids)
VNET_DEFINE(struct pf_tags, pf_qids);


#if (PF_QNAME_SIZE != PF_TAG_NAME_SIZE)
#error PF_QNAME_SIZE must be equal to PF_TAG_NAME_SIZE
#endif

static u_int16_t	 tagname2tag(struct pf_tags *, char *);
static u_int16_t	 pf_tagname2tag(char *);
void			 tag_unref(struct pf_tags *, u_int16_t);
static int		 pf_rtlabel_add(struct pf_addr_wrap *);
static void		 pf_rtlabel_remove(struct pf_addr_wrap *);
static void		 pf_rtlabel_copyout(struct pf_addr_wrap *);

#define DPFPRINTF(n, x) if (V_pf_status.debug >= (n)) printf x

struct cdev *pf_dev;
 
/*
 * XXX - These are new and need to be checked when moveing to a new version
 */
static void		 pf_clear_states(void);
static int		 pf_clear_tables(void);
static void		 pf_clear_srcnodes(void);
/*
 * XXX - These are new and need to be checked when moveing to a new version
 */
 
/*
 * Wrapper functions for pfil(9) hooks
 */
#ifdef INET
static int pf_check_in(void *arg, struct mbuf **m, struct ifnet *ifp,
    int dir, struct inpcb *inp);
static int pf_check_out(void *arg, struct mbuf **m, struct ifnet *ifp,
    int dir, struct inpcb *inp);
#endif
#ifdef INET6
static int pf_check6_in(void *arg, struct mbuf **m, struct ifnet *ifp,
    int dir, struct inpcb *inp);
static int pf_check6_out(void *arg, struct mbuf **m, struct ifnet *ifp,
    int dir, struct inpcb *inp);
#endif
 
static int		hook_pf(void);
static int		dehook_pf(void);
static int		shutdown_pf(void);
static int		pf_load(void);
static int		pf_unload(void);

static struct cdevsw pf_cdevsw = {
	.d_ioctl =	pfioctl,
	.d_name =	PF_NAME,
	.d_version =	D_VERSION,
};

static volatile VNET_DEFINE(int, pf_pfil_hooked);
#define V_pf_pfil_hooked	VNET(pf_pfil_hooked)
VNET_DEFINE(int,		pf_end_threads);

struct mtx			pf_mtx;
struct rwlock			pf_rules_lock;

/* pfsync */
pfsync_state_import_t 		*pfsync_state_import_ptr = NULL;
pfsync_insert_state_t		*pfsync_insert_state_ptr = NULL;
pfsync_update_state_t		*pfsync_update_state_ptr = NULL;
pfsync_delete_state_t		*pfsync_delete_state_ptr = NULL;
pfsync_clear_states_t		*pfsync_clear_states_ptr = NULL;
pfsync_state_in_use_t		*pfsync_state_in_use_ptr = NULL;
pfsync_defer_t			*pfsync_defer_ptr = NULL;
pfsync_up_t			*pfsync_up_ptr = NULL;
/* pflow */
export_pflow_t			*export_pflow_ptr = NULL;
/* pflog */
pflog_packet_t			*pflog_packet_ptr = NULL;

VNET_DEFINE(int, debug_pfugidhack);
SYSCTL_VNET_INT(_debug, OID_AUTO, pfugidhack, CTLFLAG_RW,
	&VNET_NAME(debug_pfugidhack), 0,
	"Enable/disable pf user/group rules mpsafe hack");

static void
init_pf_mutex(void)
{

	mtx_init(&pf_mtx, "pf Giant", NULL, MTX_DEF);
	rw_init(&pf_rules_lock, "pf rulesets");
	/* XXXGL: name */
	sx_init(&V_pf_consistency_lock, "pf_statetbl_lock");
}

static void
destroy_pf_mutex(void)
{

	mtx_destroy(&pf_mtx);
	rw_destroy(&pf_rules_lock);
	sx_destroy(&V_pf_consistency_lock);
}

static void
cleanup_pf_zone(void)
{

	uma_zdestroy(V_pf_src_tree_pl);
	uma_zdestroy(V_pf_rule_pl);
	uma_zdestroy(V_pf_state_pl);
	uma_zdestroy(V_pf_state_key_pl);
	uma_zdestroy(V_pf_state_item_pl);
	uma_zdestroy(V_pf_altq_pl);
	uma_zdestroy(V_pf_pooladdr_pl);
	uma_zdestroy(V_pfr_ktable_pl);
	uma_zdestroy(V_pfr_kentry_pl);
	uma_zdestroy(V_pfi_addr_pl);
}

int
pfattach(void)
{
	u_int32_t *my_timeout = V_pf_default_rule.timeout;

	V_pf_src_tree_pl = uma_zcreate("pfsrctrpl", sizeof(struct pf_src_node),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	V_pf_rule_pl = uma_zcreate("pfrulepl", sizeof(struct pf_rule),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	V_pf_state_pl = uma_zcreate("pfstatepl", sizeof(struct pf_state),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	V_pf_state_key_pl = uma_zcreate("pfstatekeypl",
	    sizeof(struct pf_state_key), NULL, NULL, NULL, NULL,UMA_ALIGN_PTR,
	    0);
	V_pf_state_item_pl = uma_zcreate("pfstateitempl",
	    sizeof(struct pf_state_item), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);
	V_pf_altq_pl = uma_zcreate("pfaltqpl", sizeof(struct pf_altq),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	V_pf_pooladdr_pl = uma_zcreate("pfpooladdrpl",
	    sizeof(struct pf_pooladdr), NULL, NULL, NULL, NULL, UMA_ALIGN_PTR,
	    0);
	V_pfr_ktable_pl = uma_zcreate("pfrktable", sizeof(struct pfr_ktable),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	V_pfr_kentry_pl = uma_zcreate("pfrkentry", sizeof(struct pfr_kentry),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	V_pfi_addr_pl = uma_zcreate("pfiaddrpl", sizeof(struct pfi_dynaddr),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	pfr_initialize();
	pfi_initialize();
	pf_osfp_initialize();
	pf_normalize_init();

	V_pf_pool_limits[PF_LIMIT_STATES].pp = V_pf_state_pl;
	V_pf_pool_limits[PF_LIMIT_STATES].limit = PFSTATE_HIWAT;
	V_pf_pool_limits[PF_LIMIT_SRC_NODES].pp = V_pf_src_tree_pl;
	V_pf_pool_limits[PF_LIMIT_SRC_NODES].limit = PFSNODE_HIWAT;
	V_pf_pool_limits[PF_LIMIT_TABLES].pp = V_pfr_ktable_pl;
	V_pf_pool_limits[PF_LIMIT_TABLES].limit = PFR_KTABLE_HIWAT;
	V_pf_pool_limits[PF_LIMIT_TABLE_ENTRIES].pp = V_pfr_kentry_pl;
	V_pf_pool_limits[PF_LIMIT_TABLE_ENTRIES].limit = PFR_KENTRY_HIWAT;
	uma_zone_set_max(V_pf_pool_limits[PF_LIMIT_STATES].pp,
	    V_pf_pool_limits[PF_LIMIT_STATES].limit);

	RB_INIT(&V_tree_src_tracking);
	RB_INIT(&V_pf_anchors);
	pf_init_ruleset(&pf_main_ruleset);

	TAILQ_INIT(&V_pf_altqs[0]);
	TAILQ_INIT(&V_pf_altqs[1]);
	TAILQ_INIT(&V_pf_pabuf);
	V_pf_altqs_active = &V_pf_altqs[0];
	V_pf_altqs_inactive = &V_pf_altqs[1];
	TAILQ_INIT(&V_state_list);

	/* default rule should never be garbage collected */
	V_pf_default_rule.entries.tqe_prev = &V_pf_default_rule.entries.tqe_next;
	V_pf_default_rule.action = PF_PASS;
	V_pf_default_rule.nr = -1;
	V_pf_default_rule.rtableid = -1;

	/* initialize default timeouts */
	my_timeout[PFTM_TCP_FIRST_PACKET] = PFTM_TCP_FIRST_PACKET_VAL;
	my_timeout[PFTM_TCP_OPENING] = PFTM_TCP_OPENING_VAL;
	my_timeout[PFTM_TCP_ESTABLISHED] = PFTM_TCP_ESTABLISHED_VAL;
	my_timeout[PFTM_TCP_CLOSING] = PFTM_TCP_CLOSING_VAL;
	my_timeout[PFTM_TCP_FIN_WAIT] = PFTM_TCP_FIN_WAIT_VAL;
	my_timeout[PFTM_TCP_CLOSED] = PFTM_TCP_CLOSED_VAL;
	my_timeout[PFTM_UDP_FIRST_PACKET] = PFTM_UDP_FIRST_PACKET_VAL;
	my_timeout[PFTM_UDP_SINGLE] = PFTM_UDP_SINGLE_VAL;
	my_timeout[PFTM_UDP_MULTIPLE] = PFTM_UDP_MULTIPLE_VAL;
	my_timeout[PFTM_ICMP_FIRST_PACKET] = PFTM_ICMP_FIRST_PACKET_VAL;
	my_timeout[PFTM_ICMP_ERROR_REPLY] = PFTM_ICMP_ERROR_REPLY_VAL;
	my_timeout[PFTM_OTHER_FIRST_PACKET] = PFTM_OTHER_FIRST_PACKET_VAL;
	my_timeout[PFTM_OTHER_SINGLE] = PFTM_OTHER_SINGLE_VAL;
	my_timeout[PFTM_OTHER_MULTIPLE] = PFTM_OTHER_MULTIPLE_VAL;
	my_timeout[PFTM_FRAG] = PFTM_FRAG_VAL;
	my_timeout[PFTM_INTERVAL] = PFTM_INTERVAL_VAL;
	my_timeout[PFTM_SRC_NODE] = PFTM_SRC_NODE_VAL;
	my_timeout[PFTM_TS_DIFF] = PFTM_TS_DIFF_VAL;
	my_timeout[PFTM_ADAPTIVE_START] = PFSTATE_ADAPT_START;
	my_timeout[PFTM_ADAPTIVE_END] = PFSTATE_ADAPT_END;

	bzero(&V_pf_status, sizeof(V_pf_status));
	V_pf_status.debug = PF_DEBUG_URGENT;

	V_pf_pfil_hooked = 0;

	/* XXX do our best to avoid a conflict */
	V_pf_status.hostid = arc4random();

	if (kproc_create(pf_purge_thread, curvnet, NULL, 0, 0, "pfpurge"))
		/* XXXGL: leaked all above. */
		return (ENXIO);

	m_addr_chg_pf_p = pf_pkt_addr_changed;

	return (0);
}

static struct pf_pool *
pf_get_pool(char *anchor, u_int32_t ticket, u_int8_t rule_action,
    u_int32_t rule_number, u_int8_t r_last, u_int8_t active,
    u_int8_t check_ticket)
{
	struct pf_ruleset	*ruleset;
	struct pf_rule		*rule;
	int			 rs_num;

	ruleset = pf_find_ruleset(anchor);
	if (ruleset == NULL)
		return (NULL);
	rs_num = pf_get_ruleset_number(rule_action);
	if (rs_num >= PF_RULESET_MAX)
		return (NULL);
	if (active) {
		if (check_ticket && ticket !=
		    ruleset->rules[rs_num].active.ticket)
			return (NULL);
		if (r_last)
			rule = TAILQ_LAST(ruleset->rules[rs_num].active.ptr,
			    pf_rulequeue);
		else
			rule = TAILQ_FIRST(ruleset->rules[rs_num].active.ptr);
	} else {
		if (check_ticket && ticket !=
		    ruleset->rules[rs_num].inactive.ticket)
			return (NULL);
		if (r_last)
			rule = TAILQ_LAST(ruleset->rules[rs_num].inactive.ptr,
			    pf_rulequeue);
		else
			rule = TAILQ_FIRST(ruleset->rules[rs_num].inactive.ptr);
	}
	if (!r_last) {
		while ((rule != NULL) && (rule->nr != rule_number))
			rule = TAILQ_NEXT(rule, entries);
	}
	if (rule == NULL)
		return (NULL);

	return (&rule->rpool);
}

static void
pf_mv_pool(struct pf_palist *poola, struct pf_palist *poolb)
{
	struct pf_pooladdr	*mv_pool_pa;

	while ((mv_pool_pa = TAILQ_FIRST(poola)) != NULL) {
		TAILQ_REMOVE(poola, mv_pool_pa, entries);
		TAILQ_INSERT_TAIL(poolb, mv_pool_pa, entries);
	}
}

static void
pf_empty_pool(struct pf_palist *poola)
{
	struct pf_pooladdr	*empty_pool_pa;

	while ((empty_pool_pa = TAILQ_FIRST(poola)) != NULL) {
		pfi_dynaddr_remove(&empty_pool_pa->addr);
		pf_tbladdr_remove(&empty_pool_pa->addr);
		pfi_kif_unref(empty_pool_pa->kif, PFI_KIF_REF_RULE);
		TAILQ_REMOVE(poola, empty_pool_pa, entries);
		uma_zfree(V_pf_pooladdr_pl, empty_pool_pa);
	}
}

void
pf_rm_rule(struct pf_rulequeue *rulequeue, struct pf_rule *rule)
{
	if (rulequeue != NULL) {
		if (rule->states_cur <= 0) {
			/*
			 * XXX - we need to remove the table *before* detaching
			 * the rule to make sure the table code does not delete
			 * the anchor under our feet.
			 */
			pf_tbladdr_remove(&rule->src.addr);
			pf_tbladdr_remove(&rule->dst.addr);
			if (rule->overload_tbl)
				pfr_detach_table(rule->overload_tbl);
		}
		TAILQ_REMOVE(rulequeue, rule, entries);
		rule->entries.tqe_prev = NULL;
		rule->nr = -1;
	}

	if (rule->states_cur > 0 || rule->src_nodes > 0 ||
	    rule->entries.tqe_prev != NULL)
		return;
	pf_tag_unref(rule->tag);
	pf_tag_unref(rule->match_tag);
#ifdef ALTQ
	if (rule->pqid != rule->qid)
		pf_qid_unref(rule->pqid);
	pf_qid_unref(rule->qid);
#endif
	pf_rtlabel_remove(&rule->src.addr);
	pf_rtlabel_remove(&rule->dst.addr);
	pfi_dynaddr_remove(&rule->src.addr);
	pfi_dynaddr_remove(&rule->dst.addr);
	if (rulequeue == NULL) {
		pf_tbladdr_remove(&rule->src.addr);
		pf_tbladdr_remove(&rule->dst.addr);
		if (rule->overload_tbl)
			pfr_detach_table(rule->overload_tbl);
	}
	pfi_kif_unref(rule->kif, PFI_KIF_REF_RULE);
	pf_anchor_remove(rule);
	pf_empty_pool(&rule->rpool.list);
	uma_zfree(V_pf_rule_pl, rule);
}

static u_int16_t
tagname2tag(struct pf_tags *head, char *tagname)
{
	struct pf_tagname	*tag, *p = NULL;
	u_int16_t		 new_tagid = 1;

	TAILQ_FOREACH(tag, head, entries)
		if (strcmp(tagname, tag->name) == 0) {
			tag->ref++;
			return (tag->tag);
		}

	/*
	 * to avoid fragmentation, we do a linear search from the beginning
	 * and take the first free slot we find. if there is none or the list
	 * is empty, append a new entry at the end.
	 */

	/* new entry */
	if (!TAILQ_EMPTY(head))
		for (p = TAILQ_FIRST(head); p != NULL &&
		    p->tag == new_tagid; p = TAILQ_NEXT(p, entries))
			new_tagid = p->tag + 1;

	if (new_tagid > TAGID_MAX)
		return (0);

	/* allocate and fill new struct pf_tagname */
	tag = malloc(sizeof(*tag), M_TEMP, M_NOWAIT|M_ZERO);
	if (tag == NULL)
		return (0);
	strlcpy(tag->name, tagname, sizeof(tag->name));
	tag->tag = new_tagid;
	tag->ref++;

	if (p != NULL)	/* insert new entry before p */
		TAILQ_INSERT_BEFORE(p, tag, entries);
	else	/* either list empty or no free slot in between */
		TAILQ_INSERT_TAIL(head, tag, entries);

	return (tag->tag);
}

void
tag_unref(struct pf_tags *head, u_int16_t tag)
{
	struct pf_tagname	*p, *next;

	if (tag == 0)
		return;

	for (p = TAILQ_FIRST(head); p != NULL; p = next) {
		next = TAILQ_NEXT(p, entries);
		if (tag == p->tag) {
			if (--p->ref == 0) {
				TAILQ_REMOVE(head, p, entries);
				free(p, M_TEMP);
			}
			break;
		}
	}
}

static u_int16_t
pf_tagname2tag(char *tagname)
{
	return (tagname2tag(&V_pf_tags, tagname));
}

void
pf_tag_ref(u_int16_t tag)
{
	struct pf_tagname *t;

	TAILQ_FOREACH(t, &V_pf_tags, entries)
		if (t->tag == tag)
			break;
	if (t != NULL)
		t->ref++;
}

void
pf_tag_unref(u_int16_t tag)
{
	tag_unref(&V_pf_tags, tag);
}

static int
pf_rtlabel_add(struct pf_addr_wrap *a)
{
	/* XXX_IMPORT: later */
	return (0);
}

static void
pf_rtlabel_remove(struct pf_addr_wrap *a)
{
	/* XXX_IMPORT: later */
}

static void
pf_rtlabel_copyout(struct pf_addr_wrap *a)
{
	/* XXX_IMPORT: later */
	if (a->type == PF_ADDR_RTLABEL && a->v.rtlabel)
		strlcpy(a->v.rtlabelname, "?", sizeof(a->v.rtlabelname));
}

#ifdef ALTQ
static u_int32_t
pf_qname2qid(char *qname)
{
	return ((u_int32_t)tagname2tag(&V_pf_qids, qname));
}

static void
pf_qid_unref(u_int32_t qid)
{
	tag_unref(&V_pf_qids, (u_int16_t)qid);
}

static int
pf_begin_altq(u_int32_t *ticket)
{
	struct pf_altq	*altq;
	int		 error = 0;

	/* Purge the old altq list */
	while ((altq = TAILQ_FIRST(V_pf_altqs_inactive)) != NULL) {
		TAILQ_REMOVE(V_pf_altqs_inactive, altq, entries);
		if (altq->qname[0] == 0 &&
		    (altq->local_flags & PFALTQ_FLAG_IF_REMOVED) == 0) {
			/* detach and destroy the discipline */
			error = altq_remove(altq);
		} else
			pf_qid_unref(altq->qid);
		uma_zfree(V_pf_altq_pl, altq);
	}
	if (error)
		return (error);
	*ticket = ++V_ticket_altqs_inactive;
	V_altqs_inactive_open = 1;
	return (0);
}

static int
pf_rollback_altq(u_int32_t ticket)
{
	struct pf_altq	*altq;
	int		 error = 0;

	if (!V_altqs_inactive_open || ticket != V_ticket_altqs_inactive)
		return (0);
	/* Purge the old altq list */
	while ((altq = TAILQ_FIRST(V_pf_altqs_inactive)) != NULL) {
		TAILQ_REMOVE(V_pf_altqs_inactive, altq, entries);
		if (altq->qname[0] == 0 &&
		   (altq->local_flags & PFALTQ_FLAG_IF_REMOVED) == 0) {
			/* detach and destroy the discipline */
			error = altq_remove(altq);
		} else
			pf_qid_unref(altq->qid);
		uma_zfree(V_pf_altq_pl, altq);
	}
	V_altqs_inactive_open = 0;
	return (error);
}

static int
pf_commit_altq(u_int32_t ticket)
{
	struct pf_altqqueue	*old_altqs;
	struct pf_altq		*altq;
	int			 err, error = 0;

	if (!V_altqs_inactive_open || ticket != V_ticket_altqs_inactive)
		return (EBUSY);

	/* swap altqs, keep the old. */
	old_altqs = V_pf_altqs_active;
	V_pf_altqs_active = V_pf_altqs_inactive;
	V_pf_altqs_inactive = old_altqs;
	V_ticket_altqs_active = V_ticket_altqs_inactive;

	/* Attach new disciplines */
	TAILQ_FOREACH(altq, V_pf_altqs_active, entries) {
	if (altq->qname[0] == 0 &&
	   (altq->local_flags & PFALTQ_FLAG_IF_REMOVED) == 0) {
			/* attach the discipline */
			error = altq_pfattach(altq);
			if (error == 0 && V_pf_altq_running)
				error = pf_enable_altq(altq);
			if (error != 0)
				return (error);
		}
	}

	/* Purge the old altq list */
	while ((altq = TAILQ_FIRST(V_pf_altqs_inactive)) != NULL) {
		TAILQ_REMOVE(V_pf_altqs_inactive, altq, entries);
		if (altq->qname[0] == 0 &&
		    (altq->local_flags & PFALTQ_FLAG_IF_REMOVED) == 0) {
			/* detach and destroy the discipline */
			if (V_pf_altq_running)
				error = pf_disable_altq(altq);
			err = altq_pfdetach(altq);
			if (err != 0 && error == 0)
				error = err;
			err = altq_remove(altq);
			if (err != 0 && error == 0)
				error = err;
		} else
			pf_qid_unref(altq->qid);
		uma_zfree(V_pf_altq_pl, altq);
	}

	V_altqs_inactive_open = 0;
	return (error);
}

static int
pf_enable_altq(struct pf_altq *altq)
{
	struct ifnet		*ifp;
	struct tb_profile	 tb;
	int			 error = 0;

	if ((ifp = ifunit(altq->ifname)) == NULL)
		return (EINVAL);

	if (ifp->if_snd.altq_type != ALTQT_NONE)
		error = altq_enable(&ifp->if_snd);

	/* set tokenbucket regulator */
	if (error == 0 && ifp != NULL && ALTQ_IS_ENABLED(&ifp->if_snd)) {
		tb.rate = altq->ifbandwidth;
		tb.depth = altq->tbrsize;
		PF_UNLOCK();
		error = tbr_set(&ifp->if_snd, &tb);
		PF_LOCK();
	}

	return (error);
}

static int
pf_disable_altq(struct pf_altq *altq)
{
	struct ifnet		*ifp;
	struct tb_profile	 tb;
	int			 error;

	if ((ifp = ifunit(altq->ifname)) == NULL)
		return (EINVAL);

	/*
	 * when the discipline is no longer referenced, it was overridden
	 * by a new one.  if so, just return.
	 */
	if (altq->altq_disc != ifp->if_snd.altq_disc)
		return (0);

	error = altq_disable(&ifp->if_snd);

	if (error == 0) {
		/* clear tokenbucket regulator */
		tb.rate = 0;
		PF_UNLOCK();
		error = tbr_set(&ifp->if_snd, &tb);
		PF_LOCK();
	}

	return (error);
}

void
pf_altq_ifnet_event(struct ifnet *ifp, int remove)
{
	struct ifnet	*ifp1;
	struct pf_altq	*a1, *a2, *a3;
	u_int32_t	 ticket;
	int		 error = 0;

	/* Interrupt userland queue modifications */
	if (V_altqs_inactive_open)
		pf_rollback_altq(V_ticket_altqs_inactive);

	/* Start new altq ruleset */
	if (pf_begin_altq(&ticket))
		return;

	/* Copy the current active set */
	TAILQ_FOREACH(a1, V_pf_altqs_active, entries) {
		a2 = uma_zalloc(V_pf_altq_pl, M_NOWAIT);
		if (a2 == NULL) {
			error = ENOMEM;
			break;
		}
		bcopy(a1, a2, sizeof(struct pf_altq));

		if (a2->qname[0] != 0) {
			if ((a2->qid = pf_qname2qid(a2->qname)) == 0) {
				error = EBUSY;
				uma_zfree(V_pf_altq_pl, a2);
				break;
			}
			a2->altq_disc = NULL;
			TAILQ_FOREACH(a3, V_pf_altqs_inactive, entries) {
				if (strncmp(a3->ifname, a2->ifname,
				    IFNAMSIZ) == 0 && a3->qname[0] == 0) {
					a2->altq_disc = a3->altq_disc;
					break;
				}
			}
		}
		/* Deactivate the interface in question */
		a2->local_flags &= ~PFALTQ_FLAG_IF_REMOVED;
		if ((ifp1 = ifunit(a2->ifname)) == NULL ||
		    (remove && ifp1 == ifp)) {
			a2->local_flags |= PFALTQ_FLAG_IF_REMOVED;
		} else {
			PF_UNLOCK();
			error = altq_add(a2);
			PF_LOCK();

			if (ticket != V_ticket_altqs_inactive)
				error = EBUSY;

			if (error) {
				uma_zfree(V_pf_altq_pl, a2);
				break;
			}
		}

		TAILQ_INSERT_TAIL(V_pf_altqs_inactive, a2, entries);
	}

	if (error != 0)
		pf_rollback_altq(ticket);
	else
		pf_commit_altq(ticket);
	}
#endif /* ALTQ */

static int
pf_begin_rules(u_int32_t *ticket, int rs_num, const char *anchor)
{
	struct pf_ruleset	*rs;
	struct pf_rule		*rule;

	if (rs_num < 0 || rs_num >= PF_RULESET_MAX)
		return (EINVAL);
	rs = pf_find_or_create_ruleset(anchor);
	if (rs == NULL)
		return (EINVAL);
	while ((rule = TAILQ_FIRST(rs->rules[rs_num].inactive.ptr)) != NULL) {
		pf_rm_rule(rs->rules[rs_num].inactive.ptr, rule);
		rs->rules[rs_num].inactive.rcount--;
	}
	*ticket = ++rs->rules[rs_num].inactive.ticket;
	rs->rules[rs_num].inactive.open = 1;
	return (0);
}

static int
pf_rollback_rules(u_int32_t ticket, int rs_num, char *anchor)
{
	struct pf_ruleset	*rs;
	struct pf_rule		*rule;

	if (rs_num < 0 || rs_num >= PF_RULESET_MAX)
		return (EINVAL);
	rs = pf_find_ruleset(anchor);
	if (rs == NULL || !rs->rules[rs_num].inactive.open ||
	    rs->rules[rs_num].inactive.ticket != ticket)
		return (0);
	while ((rule = TAILQ_FIRST(rs->rules[rs_num].inactive.ptr)) != NULL) {
		pf_rm_rule(rs->rules[rs_num].inactive.ptr, rule);
		rs->rules[rs_num].inactive.rcount--;
	}
	rs->rules[rs_num].inactive.open = 0;
	return (0);
}

#define PF_MD5_UPD(st, elm)						\
		MD5Update(ctx, (u_int8_t *) &(st)->elm, sizeof((st)->elm))

#define PF_MD5_UPD_STR(st, elm)						\
		MD5Update(ctx, (u_int8_t *) (st)->elm, strlen((st)->elm))

#define PF_MD5_UPD_HTONL(st, elm, stor) do {				\
		(stor) = htonl((st)->elm);				\
		MD5Update(ctx, (u_int8_t *) &(stor), sizeof(u_int32_t));\
} while (0)

#define PF_MD5_UPD_HTONS(st, elm, stor) do {				\
		(stor) = htons((st)->elm);				\
		MD5Update(ctx, (u_int8_t *) &(stor), sizeof(u_int16_t));\
} while (0)

static void
pf_hash_rule_addr(MD5_CTX *ctx, struct pf_rule_addr *pfr)
{
	PF_MD5_UPD(pfr, addr.type);
	switch (pfr->addr.type) {
		case PF_ADDR_DYNIFTL:
			PF_MD5_UPD(pfr, addr.v.ifname);
			PF_MD5_UPD(pfr, addr.iflags);
			break;
		case PF_ADDR_TABLE:
			PF_MD5_UPD(pfr, addr.v.tblname);
			break;
		case PF_ADDR_ADDRMASK:
			/* XXX ignore af? */
			PF_MD5_UPD(pfr, addr.v.a.addr.addr32);
			PF_MD5_UPD(pfr, addr.v.a.mask.addr32);
			break;
		case PF_ADDR_RTLABEL:
			PF_MD5_UPD(pfr, addr.v.rtlabelname);
			break;
	}

	PF_MD5_UPD(pfr, port[0]);
	PF_MD5_UPD(pfr, port[1]);
	PF_MD5_UPD(pfr, neg);
	PF_MD5_UPD(pfr, port_op);
}

static void
pf_hash_rule(MD5_CTX *ctx, struct pf_rule *rule)
{
	u_int16_t x;
	u_int32_t y;

	pf_hash_rule_addr(ctx, &rule->src);
	pf_hash_rule_addr(ctx, &rule->dst);
	PF_MD5_UPD_STR(rule, label);
	PF_MD5_UPD_STR(rule, ifname);
	PF_MD5_UPD_STR(rule, match_tagname);
	PF_MD5_UPD_HTONS(rule, match_tag, x); /* dup? */
	PF_MD5_UPD_HTONL(rule, os_fingerprint, y);
	PF_MD5_UPD_HTONL(rule, prob, y);
	PF_MD5_UPD_HTONL(rule, uid.uid[0], y);
	PF_MD5_UPD_HTONL(rule, uid.uid[1], y);
	PF_MD5_UPD(rule, uid.op);
	PF_MD5_UPD_HTONL(rule, gid.gid[0], y);
	PF_MD5_UPD_HTONL(rule, gid.gid[1], y);
	PF_MD5_UPD(rule, gid.op);
	PF_MD5_UPD_HTONL(rule, rule_flag, y);
	PF_MD5_UPD(rule, action);
	PF_MD5_UPD(rule, direction);
	PF_MD5_UPD(rule, af);
	PF_MD5_UPD(rule, quick);
	PF_MD5_UPD(rule, ifnot);
	PF_MD5_UPD(rule, match_tag_not);
	PF_MD5_UPD(rule, natpass);
	PF_MD5_UPD(rule, keep_state);
	PF_MD5_UPD(rule, proto);
	PF_MD5_UPD(rule, type);
	PF_MD5_UPD(rule, code);
	PF_MD5_UPD(rule, flags);
	PF_MD5_UPD(rule, flagset);
	PF_MD5_UPD(rule, allow_opts);
	PF_MD5_UPD(rule, rt);
	PF_MD5_UPD(rule, tos);
}

static int
pf_commit_rules(u_int32_t ticket, int rs_num, char *anchor)
{
	struct pf_ruleset	*rs;
	struct pf_rule		*rule, **old_array;
	struct pf_rulequeue	*old_rules;
	int			 error;
	u_int32_t		 old_rcount;

	if (rs_num < 0 || rs_num >= PF_RULESET_MAX)
		return (EINVAL);
	rs = pf_find_ruleset(anchor);
	if (rs == NULL || !rs->rules[rs_num].inactive.open ||
	    ticket != rs->rules[rs_num].inactive.ticket)
		return (EBUSY);

	/* Calculate checksum for the main ruleset */
	if (rs == &pf_main_ruleset) {
		error = pf_setup_pfsync_matching(rs);
		if (error != 0)
			return (error);
	}

	/* Swap rules, keep the old. */
	old_rules = rs->rules[rs_num].active.ptr;
	old_rcount = rs->rules[rs_num].active.rcount;
	old_array = rs->rules[rs_num].active.ptr_array;

	rs->rules[rs_num].active.ptr =
	    rs->rules[rs_num].inactive.ptr;
	rs->rules[rs_num].active.ptr_array =
	    rs->rules[rs_num].inactive.ptr_array;
	rs->rules[rs_num].active.rcount =
	    rs->rules[rs_num].inactive.rcount;
	rs->rules[rs_num].inactive.ptr = old_rules;
	rs->rules[rs_num].inactive.ptr_array = old_array;
	rs->rules[rs_num].inactive.rcount = old_rcount;

	rs->rules[rs_num].active.ticket =
	    rs->rules[rs_num].inactive.ticket;
	pf_calc_skip_steps(rs->rules[rs_num].active.ptr);


	/* Purge the old rule list. */
	while ((rule = TAILQ_FIRST(old_rules)) != NULL)
		pf_rm_rule(old_rules, rule);
	if (rs->rules[rs_num].inactive.ptr_array)
		free(rs->rules[rs_num].inactive.ptr_array, M_TEMP);
	rs->rules[rs_num].inactive.ptr_array = NULL;
	rs->rules[rs_num].inactive.rcount = 0;
	rs->rules[rs_num].inactive.open = 0;
	pf_remove_if_empty_ruleset(rs);

	return (0);
}

static int
pf_setup_pfsync_matching(struct pf_ruleset *rs)
{
	MD5_CTX			 ctx;
	struct pf_rule		*rule;
	int			 rs_cnt;
	u_int8_t		 digest[PF_MD5_DIGEST_LENGTH];

	MD5Init(&ctx);
	for (rs_cnt = 0; rs_cnt < PF_RULESET_MAX; rs_cnt++) {
		/* XXX PF_RULESET_SCRUB as well? */
		if (rs_cnt == PF_RULESET_SCRUB)
			continue;

		if (rs->rules[rs_cnt].inactive.ptr_array)
			free(rs->rules[rs_cnt].inactive.ptr_array, M_TEMP);
		rs->rules[rs_cnt].inactive.ptr_array = NULL;

		if (rs->rules[rs_cnt].inactive.rcount) {
			rs->rules[rs_cnt].inactive.ptr_array =
			    malloc(sizeof(caddr_t) *
			    rs->rules[rs_cnt].inactive.rcount,
			    M_TEMP, M_NOWAIT);

			if (!rs->rules[rs_cnt].inactive.ptr_array)
				return (ENOMEM);
		}

		TAILQ_FOREACH(rule, rs->rules[rs_cnt].inactive.ptr,
		    entries) {
			pf_hash_rule(&ctx, rule);
			(rs->rules[rs_cnt].inactive.ptr_array)[rule->nr] = rule;
		}
	}

	MD5Final(digest, &ctx);
	memcpy(V_pf_status.pf_chksum, digest, sizeof(V_pf_status.pf_chksum));
	return (0);
}

static int
pf_addr_setup(struct pf_ruleset *ruleset, struct pf_addr_wrap *addr,
    sa_family_t af)
{
	if (pfi_dynaddr_setup(addr, af) ||
	    pf_tbladdr_setup(ruleset, addr))
		return (EINVAL);

	return (0);
}

static void
pf_addr_copyout(struct pf_addr_wrap *addr)
{
	pfi_dynaddr_copyout(addr);
	pf_tbladdr_copyout(addr);
	pf_rtlabel_copyout(addr);
}

static int
pfioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flags, struct thread *td)
{
	struct pf_pooladdr	*pa = NULL;
	struct pf_pool		*pool = NULL;
	int			 error = 0;

	CURVNET_SET(TD_TO_VNET(td));

	/* XXX keep in sync with switch() below */
	if (securelevel_gt(td->td_ucred, 2))
		switch (cmd) {
		case DIOCGETRULES:
		case DIOCGETRULE:
		case DIOCGETADDRS:
		case DIOCGETADDR:
		case DIOCGETSTATE:
		case DIOCSETSTATUSIF:
		case DIOCGETSTATUS:
		case DIOCCLRSTATUS:
		case DIOCNATLOOK:
		case DIOCSETDEBUG:
		case DIOCGETSTATES:
		case DIOCGETTIMEOUT:
		case DIOCCLRRULECTRS:
		case DIOCGETLIMIT:
		case DIOCGETALTQS:
		case DIOCGETALTQ:
		case DIOCGETQSTATS:
		case DIOCGETRULESETS:
		case DIOCGETRULESET:
		case DIOCRGETTABLES:
		case DIOCRGETTSTATS:
		case DIOCRCLRTSTATS:
		case DIOCRCLRADDRS:
		case DIOCRADDADDRS:
		case DIOCRDELADDRS:
		case DIOCRSETADDRS:
		case DIOCRGETADDRS:
		case DIOCRGETASTATS:
		case DIOCRCLRASTATS:
		case DIOCRTSTADDRS:
		case DIOCOSFPGET:
		case DIOCGETSRCNODES:
		case DIOCCLRSRCNODES:
		case DIOCIGETIFACES:
		case DIOCGIFSPEED:
		case DIOCSETIFFLAG:
		case DIOCCLRIFFLAG:
			break;
		case DIOCRCLRTABLES:
		case DIOCRADDTABLES:
		case DIOCRDELTABLES:
		case DIOCRSETTFLAGS:
			if (((struct pfioc_table *)addr)->pfrio_flags &
			    PFR_FLAG_DUMMY)
				break; /* dummy operation ok */
			return (EPERM);
		default:
			return (EPERM);
		}

	if (!(flags & FWRITE))
		switch (cmd) {
		case DIOCGETRULES:
		case DIOCGETADDRS:
		case DIOCGETADDR:
		case DIOCGETSTATE:
		case DIOCGETSTATUS:
		case DIOCGETSTATES:
		case DIOCGETTIMEOUT:
		case DIOCGETLIMIT:
		case DIOCGETALTQS:
		case DIOCGETALTQ:
		case DIOCGETQSTATS:
		case DIOCGETRULESETS:
		case DIOCGETRULESET:
		case DIOCNATLOOK:
		case DIOCRGETTABLES:
		case DIOCRGETTSTATS:
		case DIOCRGETADDRS:
		case DIOCRGETASTATS:
		case DIOCRTSTADDRS:
		case DIOCOSFPGET:
		case DIOCGETSRCNODES:
		case DIOCIGETIFACES:
		case DIOCGIFSPEED:
			break;
		case DIOCRCLRTABLES:
		case DIOCRADDTABLES:
		case DIOCRDELTABLES:
		case DIOCRCLRTSTATS:
		case DIOCRCLRADDRS:
		case DIOCRADDADDRS:
		case DIOCRDELADDRS:
		case DIOCRSETADDRS:
		case DIOCRSETTFLAGS:
			if (((struct pfioc_table *)addr)->pfrio_flags &
			    PFR_FLAG_DUMMY) {
				flags |= FWRITE; /* need write lock for dummy */
				break; /* dummy operation ok */
			}
			return (EACCES);
		case DIOCGETRULE:
			if (((struct pfioc_rule *)addr)->action ==
			    PF_GET_CLR_CNTR)
				return (EACCES);
			break;
		default:
			return (EACCES);
		}

	if (flags & FWRITE)
		sx_xlock(&V_pf_consistency_lock);
	else
		sx_slock(&V_pf_consistency_lock);

	PF_LOCK();
	switch (cmd) {

	case DIOCSTART:
		if (V_pf_status.running)
			error = EEXIST;
		else {
			PF_UNLOCK();
			error = hook_pf();
			PF_LOCK();
			if (error) {
				DPFPRINTF(PF_DEBUG_MISC,
				    ("pf: pfil registeration fail\n"));
				break;
			}
			V_pf_status.running = 1;
			V_pf_status.since = time_second;

			if (V_pf_status.stateid == 0) {
				V_pf_status.stateid = time_second;
				V_pf_status.stateid = V_pf_status.stateid << 32;
			}
			DPFPRINTF(PF_DEBUG_MISC, ("pf: started\n"));
		}
		break;

	case DIOCSTOP:
		if (!V_pf_status.running)
			error = ENOENT;
		else {
			V_pf_status.running = 0;
			PF_UNLOCK();
			error = dehook_pf();
			PF_LOCK();
			if (error) {
				V_pf_status.running = 1;
				DPFPRINTF(PF_DEBUG_MISC,
				    ("pf: pfil unregisteration failed\n"));
			}
			V_pf_status.since = time_second;
			DPFPRINTF(PF_DEBUG_MISC, ("pf: stopped\n"));
		}
		break;

	case DIOCADDRULE: {
		struct pfioc_rule	*pr = (struct pfioc_rule *)addr;
		struct pf_ruleset	*ruleset;
		struct pf_rule		*rule, *tail;
		struct pf_pooladdr	*pa;
		int			 rs_num;

		pr->anchor[sizeof(pr->anchor) - 1] = 0;
		ruleset = pf_find_ruleset(pr->anchor);
		if (ruleset == NULL) {
			error = EINVAL;
			break;
		}
		rs_num = pf_get_ruleset_number(pr->rule.action);
		if (rs_num >= PF_RULESET_MAX) {
			error = EINVAL;
			break;
		}
		if (pr->rule.return_icmp >> 8 > ICMP_MAXTYPE) {
			error = EINVAL;
			break;
		}
		if (pr->ticket != ruleset->rules[rs_num].inactive.ticket) {
			DPFPRINTF(PF_DEBUG_MISC,
			    ("ticket: %d != [%d]%d\n", pr->ticket, rs_num,
			    ruleset->rules[rs_num].inactive.ticket));
			error = EBUSY;
			break;
		}
		if (pr->pool_ticket != V_ticket_pabuf) {
			DPFPRINTF(PF_DEBUG_MISC,
			    ("pool_ticket: %d != %d\n", pr->pool_ticket,
			    V_ticket_pabuf));
			error = EBUSY;
			break;
		}
		rule = uma_zalloc(V_pf_rule_pl, M_NOWAIT);
		if (rule == NULL) {
			error = ENOMEM;
			break;
		}
		bcopy(&pr->rule, rule, sizeof(struct pf_rule));
		rule->cuid = td->td_ucred->cr_ruid;
		rule->cpid = td->td_proc ? td->td_proc->p_pid : 0;
		rule->anchor = NULL;
		rule->kif = NULL;
		TAILQ_INIT(&rule->rpool.list);
		/* initialize refcounting */
		rule->states_cur = 0;
		rule->src_nodes = 0;
		rule->entries.tqe_prev = NULL;
#ifndef INET
		if (rule->af == AF_INET) {
			uma_zfree(V_pf_rule_pl, rule);
			error = EAFNOSUPPORT;
			break;
		}
#endif /* INET */
#ifndef INET6
		if (rule->af == AF_INET6) {
			uma_zfree(V_pf_rule_pl, rule);
			error = EAFNOSUPPORT;
			break;
		}
#endif /* INET6 */
		tail = TAILQ_LAST(ruleset->rules[rs_num].inactive.ptr,
		    pf_rulequeue);
		if (tail)
			rule->nr = tail->nr + 1;
		else
			rule->nr = 0;
		if (rule->ifname[0]) {
			rule->kif = pfi_kif_get(rule->ifname);
			if (rule->kif == NULL) {
				uma_zfree(V_pf_rule_pl, rule);
				error = EINVAL;
				break;
			}
			pfi_kif_ref(rule->kif, PFI_KIF_REF_RULE);
		}

		if (rule->rtableid > 0 && rule->rtableid >= rt_numfibs)
			error = EBUSY;

#ifdef ALTQ
		/* set queue IDs */
		if (rule->qname[0] != 0) {
			if ((rule->qid = pf_qname2qid(rule->qname)) == 0)
				error = EBUSY;
			else if (rule->pqname[0] != 0) {
				if ((rule->pqid =
				    pf_qname2qid(rule->pqname)) == 0)
					error = EBUSY;
			} else
				rule->pqid = rule->qid;
		}
#endif
		if (rule->tagname[0])
			if ((rule->tag = pf_tagname2tag(rule->tagname)) == 0)
				error = EBUSY;
		if (rule->match_tagname[0])
			if ((rule->match_tag =
			    pf_tagname2tag(rule->match_tagname)) == 0)
				error = EBUSY;
		if (rule->rt && !rule->direction)
			error = EINVAL;
#if NPFLOG > 0
		if (!rule->log)
			rule->logif = 0;
		if (rule->logif >= PFLOGIFS_MAX)
			error = EINVAL;
#endif
		if (pf_rtlabel_add(&rule->src.addr) ||
		    pf_rtlabel_add(&rule->dst.addr))
			error = EBUSY;
		if (pf_addr_setup(ruleset, &rule->src.addr, rule->af))
			error = EINVAL;
		if (pf_addr_setup(ruleset, &rule->dst.addr, rule->af))
			error = EINVAL;
		if (pf_anchor_setup(rule, ruleset, pr->anchor_call))
			error = EINVAL;
		TAILQ_FOREACH(pa, &V_pf_pabuf, entries)
			if (pf_tbladdr_setup(ruleset, &pa->addr))
				error = EINVAL;

		if (rule->overload_tblname[0]) {
			if ((rule->overload_tbl = pfr_attach_table(ruleset,
			    rule->overload_tblname, 0)) == NULL)
				error = EINVAL;
			else
				rule->overload_tbl->pfrkt_flags |=
				    PFR_TFLAG_ACTIVE;
		}

		pf_mv_pool(&V_pf_pabuf, &rule->rpool.list);
		if (((((rule->action == PF_NAT) || (rule->action == PF_RDR) ||
		    (rule->action == PF_BINAT)) && rule->anchor == NULL) ||
		    (rule->rt > PF_FASTROUTE)) &&
		    (TAILQ_FIRST(&rule->rpool.list) == NULL))
			error = EINVAL;

		if (error) {
			pf_rm_rule(NULL, rule);
			break;
		}

		if (!V_debug_pfugidhack && (rule->uid.op || rule->gid.op ||
		    rule->log & PF_LOG_SOCKET_LOOKUP)) {
			DPFPRINTF(PF_DEBUG_MISC,
			    ("pf: debug.pfugidhack enabled\n"));
			V_debug_pfugidhack = 1;
		}
		rule->rpool.cur = TAILQ_FIRST(&rule->rpool.list);
		rule->evaluations = rule->packets[0] = rule->packets[1] =
		    rule->bytes[0] = rule->bytes[1] = 0;
		TAILQ_INSERT_TAIL(ruleset->rules[rs_num].inactive.ptr,
		    rule, entries);
		ruleset->rules[rs_num].inactive.rcount++;
		break;
	}

	case DIOCGETRULES: {
		struct pfioc_rule	*pr = (struct pfioc_rule *)addr;
		struct pf_ruleset	*ruleset;
		struct pf_rule		*tail;
		int			 rs_num;

		pr->anchor[sizeof(pr->anchor) - 1] = 0;
		ruleset = pf_find_ruleset(pr->anchor);
		if (ruleset == NULL) {
			error = EINVAL;
			break;
		}
		rs_num = pf_get_ruleset_number(pr->rule.action);
		if (rs_num >= PF_RULESET_MAX) {
			error = EINVAL;
			break;
		}
		tail = TAILQ_LAST(ruleset->rules[rs_num].active.ptr,
		    pf_rulequeue);
		if (tail)
			pr->nr = tail->nr + 1;
		else
			pr->nr = 0;
		pr->ticket = ruleset->rules[rs_num].active.ticket;
		break;
	}

	case DIOCGETRULE: {
		struct pfioc_rule	*pr = (struct pfioc_rule *)addr;
		struct pf_ruleset	*ruleset;
		struct pf_rule		*rule;
		int			 rs_num, i;

		pr->anchor[sizeof(pr->anchor) - 1] = 0;
		ruleset = pf_find_ruleset(pr->anchor);
		if (ruleset == NULL) {
			error = EINVAL;
			break;
		}
		rs_num = pf_get_ruleset_number(pr->rule.action);
		if (rs_num >= PF_RULESET_MAX) {
			error = EINVAL;
			break;
		}
		if (pr->ticket != ruleset->rules[rs_num].active.ticket) {
			error = EBUSY;
			break;
		}
		rule = TAILQ_FIRST(ruleset->rules[rs_num].active.ptr);
		while ((rule != NULL) && (rule->nr != pr->nr))
			rule = TAILQ_NEXT(rule, entries);
		if (rule == NULL) {
			error = EBUSY;
			break;
		}
		bcopy(rule, &pr->rule, sizeof(struct pf_rule));
		if (pf_anchor_copyout(ruleset, rule, pr)) {
			error = EBUSY;
			break;
		}
		pf_addr_copyout(&pr->rule.src.addr);
		pf_addr_copyout(&pr->rule.dst.addr);
		for (i = 0; i < PF_SKIP_COUNT; ++i)
			if (rule->skip[i].ptr == NULL)
				pr->rule.skip[i].nr = -1;
			else
				pr->rule.skip[i].nr =
				    rule->skip[i].ptr->nr;

		if (pr->action == PF_GET_CLR_CNTR) {
			rule->evaluations = 0;
			rule->packets[0] = rule->packets[1] = 0;
			rule->bytes[0] = rule->bytes[1] = 0;
			rule->states_tot = 0;
		}
		break;
	}

	case DIOCCHANGERULE: {
		struct pfioc_rule	*pcr = (struct pfioc_rule *)addr;
		struct pf_ruleset	*ruleset;
		struct pf_rule		*oldrule = NULL, *newrule = NULL;
		u_int32_t		 nr = 0;
		int			 rs_num;

		if (!(pcr->action == PF_CHANGE_REMOVE ||
		    pcr->action == PF_CHANGE_GET_TICKET) &&
		    pcr->pool_ticket != V_ticket_pabuf) {
			error = EBUSY;
			break;
		}

		if (pcr->action < PF_CHANGE_ADD_HEAD ||
		    pcr->action > PF_CHANGE_GET_TICKET) {
			error = EINVAL;
			break;
		}
		ruleset = pf_find_ruleset(pcr->anchor);
		if (ruleset == NULL) {
			error = EINVAL;
			break;
		}
		rs_num = pf_get_ruleset_number(pcr->rule.action);
		if (rs_num >= PF_RULESET_MAX) {
			error = EINVAL;
			break;
		}

		if (pcr->action == PF_CHANGE_GET_TICKET) {
			pcr->ticket = ++ruleset->rules[rs_num].active.ticket;
			break;
		} else {
			if (pcr->ticket !=
			    ruleset->rules[rs_num].active.ticket) {
				error = EINVAL;
				break;
			}
			if (pcr->rule.return_icmp >> 8 > ICMP_MAXTYPE) {
				error = EINVAL;
				break;
			}
		}

		if (pcr->action != PF_CHANGE_REMOVE) {
			newrule = uma_zalloc(V_pf_rule_pl, M_NOWAIT);
			if (newrule == NULL) {
				error = ENOMEM;
				break;
			}
			bcopy(&pcr->rule, newrule, sizeof(struct pf_rule));
			newrule->cuid = td->td_ucred->cr_ruid;
			newrule->cpid = td->td_proc ? td->td_proc->p_pid : 0;
			TAILQ_INIT(&newrule->rpool.list);
			/* initialize refcounting */
			newrule->states_cur = 0;
			newrule->entries.tqe_prev = NULL;
#ifndef INET
			if (newrule->af == AF_INET) {
				uma_zfree(V_pf_rule_pl, newrule);
				error = EAFNOSUPPORT;
				break;
			}
#endif /* INET */
#ifndef INET6
			if (newrule->af == AF_INET6) {
				uma_zfree(V_pf_rule_pl, newrule);
				error = EAFNOSUPPORT;
				break;
			}
#endif /* INET6 */
			if (newrule->ifname[0]) {
				newrule->kif = pfi_kif_get(newrule->ifname);
				if (newrule->kif == NULL) {
					uma_zfree(V_pf_rule_pl, newrule);
					error = EINVAL;
					break;
				}
				pfi_kif_ref(newrule->kif, PFI_KIF_REF_RULE);
			} else
				newrule->kif = NULL;

			if (newrule->rtableid > 0 &&
			    newrule->rtableid >= rt_numfibs)
				error = EBUSY;

#ifdef ALTQ
			/* set queue IDs */
			if (newrule->qname[0] != 0) {
				if ((newrule->qid =
				    pf_qname2qid(newrule->qname)) == 0)
					error = EBUSY;
				else if (newrule->pqname[0] != 0) {
					if ((newrule->pqid =
					    pf_qname2qid(newrule->pqname)) == 0)
						error = EBUSY;
				} else
					newrule->pqid = newrule->qid;
			}
#endif /* ALTQ */
			if (newrule->tagname[0])
				if ((newrule->tag =
				    pf_tagname2tag(newrule->tagname)) == 0)
					error = EBUSY;
			if (newrule->match_tagname[0])
				if ((newrule->match_tag = pf_tagname2tag(
				    newrule->match_tagname)) == 0)
					error = EBUSY;
			if (newrule->rt && !newrule->direction)
				error = EINVAL;
#if NPFLOG > 0
			if (!newrule->log)
				newrule->logif = 0;
			if (newrule->logif >= PFLOGIFS_MAX)
				error = EINVAL;
#endif
			if (pf_rtlabel_add(&newrule->src.addr) ||
			    pf_rtlabel_add(&newrule->dst.addr))
				error = EBUSY;
			if (pf_addr_setup(ruleset, &newrule->src.addr, newrule->af))
				error = EINVAL;
			if (pf_addr_setup(ruleset, &newrule->dst.addr, newrule->af))
				error = EINVAL;
			if (pf_anchor_setup(newrule, ruleset, pcr->anchor_call))
				error = EINVAL;
			TAILQ_FOREACH(pa, &V_pf_pabuf, entries)
				if (pf_tbladdr_setup(ruleset, &pa->addr))
					error = EINVAL;

			if (newrule->overload_tblname[0]) {
				if ((newrule->overload_tbl = pfr_attach_table(
				    ruleset, newrule->overload_tblname, 0)) ==
				    NULL)
					error = EINVAL;
				else
					newrule->overload_tbl->pfrkt_flags |=
					    PFR_TFLAG_ACTIVE;
			}

			pf_mv_pool(&V_pf_pabuf, &newrule->rpool.list);
			if (((((newrule->action == PF_NAT) ||
			    (newrule->action == PF_RDR) ||
			    (newrule->action == PF_BINAT) ||
			    (newrule->rt > PF_FASTROUTE)) &&
			    !newrule->anchor)) &&
			    (TAILQ_FIRST(&newrule->rpool.list) == NULL))
				error = EINVAL;

			if (error) {
				pf_rm_rule(NULL, newrule);
				break;
			}

			if (!V_debug_pfugidhack && (newrule->uid.op ||
			    newrule->gid.op ||
			    newrule->log & PF_LOG_SOCKET_LOOKUP)) {
				DPFPRINTF(PF_DEBUG_MISC,
				    ("pf: debug.pfugidhack enabled\n"));
				V_debug_pfugidhack = 1;
			}

			newrule->rpool.cur = TAILQ_FIRST(&newrule->rpool.list);
			newrule->evaluations = 0;
			newrule->packets[0] = newrule->packets[1] = 0;
			newrule->bytes[0] = newrule->bytes[1] = 0;
		}
		pf_empty_pool(&V_pf_pabuf);

		if (pcr->action == PF_CHANGE_ADD_HEAD)
			oldrule = TAILQ_FIRST(
			    ruleset->rules[rs_num].active.ptr);
		else if (pcr->action == PF_CHANGE_ADD_TAIL)
			oldrule = TAILQ_LAST(
			    ruleset->rules[rs_num].active.ptr, pf_rulequeue);
		else {
			oldrule = TAILQ_FIRST(
			    ruleset->rules[rs_num].active.ptr);
			while ((oldrule != NULL) && (oldrule->nr != pcr->nr))
				oldrule = TAILQ_NEXT(oldrule, entries);
			if (oldrule == NULL) {
				if (newrule != NULL)
					pf_rm_rule(NULL, newrule);
				error = EINVAL;
				break;
			}
		}

		if (pcr->action == PF_CHANGE_REMOVE) {
			pf_rm_rule(ruleset->rules[rs_num].active.ptr, oldrule);
			ruleset->rules[rs_num].active.rcount--;
		} else {
			if (oldrule == NULL)
				TAILQ_INSERT_TAIL(
				    ruleset->rules[rs_num].active.ptr,
				    newrule, entries);
			else if (pcr->action == PF_CHANGE_ADD_HEAD ||
			    pcr->action == PF_CHANGE_ADD_BEFORE)
				TAILQ_INSERT_BEFORE(oldrule, newrule, entries);
			else
				TAILQ_INSERT_AFTER(
				    ruleset->rules[rs_num].active.ptr,
				    oldrule, newrule, entries);
			ruleset->rules[rs_num].active.rcount++;
		}

		nr = 0;
		TAILQ_FOREACH(oldrule,
		    ruleset->rules[rs_num].active.ptr, entries)
			oldrule->nr = nr++;

		ruleset->rules[rs_num].active.ticket++;

		pf_calc_skip_steps(ruleset->rules[rs_num].active.ptr);
		pf_remove_if_empty_ruleset(ruleset);

		break;
	}

	case DIOCCLRSTATES: {
		struct pf_state		*s, *nexts;
		struct pfioc_state_kill *psk = (struct pfioc_state_kill *)addr;
		u_int			 killed = 0;

		for (s = RB_MIN(pf_state_tree_id, &V_tree_id); s; s = nexts) {
			nexts = RB_NEXT(pf_state_tree_id, &V_tree_id, s);

			if (!psk->psk_ifname[0] || !strcmp(psk->psk_ifname,
			    s->kif->pfik_name)) {
				/* don't send out individual delete messages */
				SET(s->state_flags, PFSTATE_NOSYNC);
				pf_unlink_state(s);
				killed++;
			}
		}
		psk->psk_killed = killed;
		if (pfsync_clear_states_ptr != NULL)
			pfsync_clear_states_ptr(V_pf_status.hostid, psk->psk_ifname);
		break;
	}

	case DIOCKILLSTATES: {
		struct pf_state		*s, *nexts;
		struct pf_state_key	*sk;
		struct pf_addr		*srcaddr, *dstaddr;
		u_int16_t		 srcport, dstport;
		struct pfioc_state_kill	*psk = (struct pfioc_state_kill *)addr;
		u_int			 killed = 0;

		if (psk->psk_pfcmp.id) {
			if (psk->psk_pfcmp.creatorid == 0)
				psk->psk_pfcmp.creatorid = V_pf_status.hostid;
			if ((s = pf_find_state_byid(&psk->psk_pfcmp))) {
				pf_unlink_state(s);
				psk->psk_killed = 1;
			}
			break;
		}

		for (s = RB_MIN(pf_state_tree_id, &V_tree_id); s;
		    s = nexts) {
			nexts = RB_NEXT(pf_state_tree_id, &V_tree_id, s);
			sk = s->key[PF_SK_WIRE];

			if (s->direction == PF_OUT) {
				srcaddr = &sk->addr[1];
				dstaddr = &sk->addr[0];
				srcport = sk->port[0];
				dstport = sk->port[0];
			} else {
				srcaddr = &sk->addr[0];
				dstaddr = &sk->addr[1];
				srcport = sk->port[0];
				dstport = sk->port[0];
			}
			if ((!psk->psk_af || sk->af == psk->psk_af)
			    && (!psk->psk_proto || psk->psk_proto ==
			    sk->proto) &&
			    PF_MATCHA(psk->psk_src.neg,
			    &psk->psk_src.addr.v.a.addr,
			    &psk->psk_src.addr.v.a.mask,
			    srcaddr, sk->af) &&
			    PF_MATCHA(psk->psk_dst.neg,
			    &psk->psk_dst.addr.v.a.addr,
			    &psk->psk_dst.addr.v.a.mask,
			    dstaddr, sk->af) &&
			    (psk->psk_src.port_op == 0 ||
			    pf_match_port(psk->psk_src.port_op,
			    psk->psk_src.port[0], psk->psk_src.port[1],
			    srcport)) &&
			    (psk->psk_dst.port_op == 0 ||
			    pf_match_port(psk->psk_dst.port_op,
			    psk->psk_dst.port[0], psk->psk_dst.port[1],
			    dstport)) &&
			    (!psk->psk_label[0] || (s->rule.ptr->label[0] &&
			    !strcmp(psk->psk_label, s->rule.ptr->label))) &&
			    (!psk->psk_ifname[0] || !strcmp(psk->psk_ifname,
			    s->kif->pfik_name))) {
				pf_unlink_state(s);
				killed++;
			}
		}
		psk->psk_killed = killed;
		break;
	}

	case DIOCADDSTATE: {
		struct pfioc_state	*ps = (struct pfioc_state *)addr;
		struct pfsync_state	*sp = &ps->state;

		if (sp->timeout >= PFTM_MAX &&
		    sp->timeout != PFTM_UNTIL_PACKET) {
			error = EINVAL;
			break;
		}
		if (pfsync_state_import_ptr != NULL)
			error = pfsync_state_import_ptr(sp, PFSYNC_SI_IOCTL);
		break;
	}

	case DIOCGETSTATE: {
		struct pfioc_state	*ps = (struct pfioc_state *)addr;
		struct pf_state		*s;
		struct pf_state_cmp	 id_key;

		bcopy(ps->state.id, &id_key.id, sizeof(id_key.id));
		id_key.creatorid = ps->state.creatorid;

		s = pf_find_state_byid(&id_key);
		if (s == NULL) {
			error = ENOENT;
			break;
		}

		pfsync_state_export(&ps->state, s);
		break;
	}

	case DIOCGETSTATES: {
		struct pfioc_states	*ps = (struct pfioc_states *)addr;
		struct pf_state		*state;
		struct pfsync_state	*p, *pstore;
		u_int32_t		 nr = 0;

		if (ps->ps_len == 0) {
			nr = V_pf_status.states;
			ps->ps_len = sizeof(struct pfsync_state) * nr;
			break;
		}

		PF_UNLOCK();
		pstore = malloc(sizeof(*pstore), M_TEMP, M_WAITOK);
		PF_LOCK();

		p = ps->ps_states;

		state = TAILQ_FIRST(&V_state_list);
		while (state) {
			if (state->timeout != PFTM_UNLINKED) {
				if ((nr+1) * sizeof(*p) > (unsigned)ps->ps_len)
					break;
				pfsync_state_export(pstore, state);
				PF_COPYOUT(pstore, p, sizeof(*p), error);
				if (error) {
					free(pstore, M_TEMP);
					goto fail;
				}
				p++;
				nr++;
			}
			state = TAILQ_NEXT(state, entry_list);
		}

		ps->ps_len = sizeof(struct pfsync_state) * nr;

		free(pstore, M_TEMP);
		break;
	}

	case DIOCGETSTATUS: {
		struct pf_status *s = (struct pf_status *)addr;
		bcopy(&V_pf_status, s, sizeof(struct pf_status));
		pfi_update_status(s->ifname, s);
		break;
	}

	case DIOCSETSTATUSIF: {
		struct pfioc_if	*pi = (struct pfioc_if *)addr;

		if (pi->ifname[0] == 0) {
			bzero(V_pf_status.ifname, IFNAMSIZ);
			break;
		}
		strlcpy(V_pf_status.ifname, pi->ifname, IFNAMSIZ);
		break;
	}

	case DIOCCLRSTATUS: {
		bzero(V_pf_status.counters, sizeof(V_pf_status.counters));
		bzero(V_pf_status.fcounters, sizeof(V_pf_status.fcounters));
		bzero(V_pf_status.scounters, sizeof(V_pf_status.scounters));
		V_pf_status.since = time_second;
		if (*V_pf_status.ifname)
			pfi_update_status(V_pf_status.ifname, NULL);
		break;
	}

	case DIOCNATLOOK: {
		struct pfioc_natlook	*pnl = (struct pfioc_natlook *)addr;
		struct pf_state_key	*sk;
		struct pf_state		*state;
		struct pf_state_key_cmp	 key;
		int			 m = 0, direction = pnl->direction;
		int			 sidx, didx;

		/* NATLOOK src and dst are reversed, so reverse sidx/didx */
		sidx = (direction == PF_IN) ? 1 : 0;
		didx = (direction == PF_IN) ? 0 : 1;

		if (!pnl->proto ||
		    PF_AZERO(&pnl->saddr, pnl->af) ||
		    PF_AZERO(&pnl->daddr, pnl->af) ||
		    ((pnl->proto == IPPROTO_TCP ||
		    pnl->proto == IPPROTO_UDP) &&
		    (!pnl->dport || !pnl->sport)))
			error = EINVAL;
		else {
			key.af = pnl->af;
			key.proto = pnl->proto;
			PF_ACPY(&key.addr[sidx], &pnl->saddr, pnl->af);
			key.port[sidx] = pnl->sport;
			PF_ACPY(&key.addr[didx], &pnl->daddr, pnl->af);
			key.port[didx] = pnl->dport;

			state = pf_find_state_all(&key, direction, &m);

			if (m > 1)
				error = E2BIG;	/* more than one state */
			else if (state != NULL) {
				sk = state->key[sidx];
				PF_ACPY(&pnl->rsaddr, &sk->addr[sidx], sk->af);
				pnl->rsport = sk->port[sidx];
				PF_ACPY(&pnl->rdaddr, &sk->addr[didx], sk->af);
				pnl->rdport = sk->port[didx];
			} else
				error = ENOENT;
		}
		break;
	}

	case DIOCSETTIMEOUT: {
		struct pfioc_tm	*pt = (struct pfioc_tm *)addr;
		int		 old;

		if (pt->timeout < 0 || pt->timeout >= PFTM_MAX ||
		    pt->seconds < 0) {
			error = EINVAL;
			goto fail;
		}
		old = V_pf_default_rule.timeout[pt->timeout];
		if (pt->timeout == PFTM_INTERVAL && pt->seconds == 0)
			pt->seconds = 1;
		V_pf_default_rule.timeout[pt->timeout] = pt->seconds;
		if (pt->timeout == PFTM_INTERVAL && pt->seconds < old)
			wakeup(pf_purge_thread);
		pt->seconds = old;
		break;
	}

	case DIOCGETTIMEOUT: {
		struct pfioc_tm	*pt = (struct pfioc_tm *)addr;

		if (pt->timeout < 0 || pt->timeout >= PFTM_MAX) {
			error = EINVAL;
			goto fail;
		}
		pt->seconds = V_pf_default_rule.timeout[pt->timeout];
		break;
	}

	case DIOCGETLIMIT: {
		struct pfioc_limit	*pl = (struct pfioc_limit *)addr;

		if (pl->index < 0 || pl->index >= PF_LIMIT_MAX) {
			error = EINVAL;
			goto fail;
		}
		pl->limit = V_pf_pool_limits[pl->index].limit;
		break;
	}

	case DIOCSETLIMIT: {
		struct pfioc_limit	*pl = (struct pfioc_limit *)addr;
		int			 old_limit;

		if (pl->index < 0 || pl->index >= PF_LIMIT_MAX ||
		    V_pf_pool_limits[pl->index].pp == NULL) {
			error = EINVAL;
			goto fail;
		}
		uma_zone_set_max(V_pf_pool_limits[pl->index].pp, pl->limit);
		old_limit = V_pf_pool_limits[pl->index].limit;
		V_pf_pool_limits[pl->index].limit = pl->limit;
		pl->limit = old_limit;
		break;
	}

	case DIOCSETDEBUG: {
		u_int32_t	*level = (u_int32_t *)addr;

		V_pf_status.debug = *level;
		break;
	}

	case DIOCCLRRULECTRS: {
		/* obsoleted by DIOCGETRULE with action=PF_GET_CLR_CNTR */
		struct pf_ruleset	*ruleset = &pf_main_ruleset;
		struct pf_rule		*rule;

		TAILQ_FOREACH(rule,
		    ruleset->rules[PF_RULESET_FILTER].active.ptr, entries) {
			rule->evaluations = 0;
			rule->packets[0] = rule->packets[1] = 0;
			rule->bytes[0] = rule->bytes[1] = 0;
		}
		break;
	}

	case DIOCGIFSPEED: {
		struct pf_ifspeed	*psp = (struct pf_ifspeed *)addr;
		struct pf_ifspeed	ps;
		struct ifnet		*ifp;

		if (psp->ifname[0] != 0) {
			/* Can we completely trust user-land? */
			strlcpy(ps.ifname, psp->ifname, IFNAMSIZ);
			ifp = ifunit(ps.ifname);
			if (ifp != NULL)
				psp->baudrate = ifp->if_baudrate;
			else
				error = EINVAL;
		} else
			error = EINVAL;
		break;
	}

#ifdef ALTQ
	case DIOCSTARTALTQ: {
		struct pf_altq		*altq;

		/* enable all altq interfaces on active list */
		TAILQ_FOREACH(altq, V_pf_altqs_active, entries) {
			if (altq->qname[0] == 0 && (altq->local_flags &
			    PFALTQ_FLAG_IF_REMOVED) == 0) {
				error = pf_enable_altq(altq);
				if (error != 0)
					break;
			}
		}
		if (error == 0)
			V_pf_altq_running = 1;
		DPFPRINTF(PF_DEBUG_MISC, ("altq: started\n"));
		break;
	}

	case DIOCSTOPALTQ: {
		struct pf_altq		*altq;

		/* disable all altq interfaces on active list */
		TAILQ_FOREACH(altq, V_pf_altqs_active, entries) {
			if (altq->qname[0] == 0 && (altq->local_flags &
			    PFALTQ_FLAG_IF_REMOVED) == 0) {
				error = pf_disable_altq(altq);
				if (error != 0)
					break;
			}
		}
		if (error == 0)
			V_pf_altq_running = 0;
		DPFPRINTF(PF_DEBUG_MISC, ("altq: stopped\n"));
		break;
	}

	case DIOCADDALTQ: {
		struct pfioc_altq	*pa = (struct pfioc_altq *)addr;
		struct pf_altq		*altq, *a;

		if (pa->ticket != V_ticket_altqs_inactive) {
			error = EBUSY;
			break;
		}
		altq = uma_zalloc(V_pf_altq_pl, M_NOWAIT);
		if (altq == NULL) {
			error = ENOMEM;
			break;
		}
		bcopy(&pa->altq, altq, sizeof(struct pf_altq));
		altq->local_flags = 0;

		/*
		 * if this is for a queue, find the discipline and
		 * copy the necessary fields
		 */
		if (altq->qname[0] != 0) {
			if ((altq->qid = pf_qname2qid(altq->qname)) == 0) {
				error = EBUSY;
				uma_zfree(V_pf_altq_pl, altq);
				break;
			}
			altq->altq_disc = NULL;
			TAILQ_FOREACH(a, V_pf_altqs_inactive, entries) {
				if (strncmp(a->ifname, altq->ifname,
				    IFNAMSIZ) == 0 && a->qname[0] == 0) {
					altq->altq_disc = a->altq_disc;
					break;
				}
			}
		}

		struct ifnet *ifp;

		if ((ifp = ifunit(altq->ifname)) == NULL) {
			altq->local_flags |= PFALTQ_FLAG_IF_REMOVED;
		} else {
			PF_UNLOCK();
		error = altq_add(altq);
			PF_LOCK();
		}
		if (error) {
			uma_zfree(V_pf_altq_pl, altq);
			break;
		}

		TAILQ_INSERT_TAIL(V_pf_altqs_inactive, altq, entries);
		bcopy(altq, &pa->altq, sizeof(struct pf_altq));
		break;
	}

	case DIOCGETALTQS: {
		struct pfioc_altq	*pa = (struct pfioc_altq *)addr;
		struct pf_altq		*altq;

		pa->nr = 0;
		TAILQ_FOREACH(altq, V_pf_altqs_active, entries)
			pa->nr++;
		pa->ticket = V_ticket_altqs_active;
		break;
	}

	case DIOCGETALTQ: {
		struct pfioc_altq	*pa = (struct pfioc_altq *)addr;
		struct pf_altq		*altq;
		u_int32_t		 nr;

		if (pa->ticket != V_ticket_altqs_active) {
			error = EBUSY;
			break;
		}
		nr = 0;
		altq = TAILQ_FIRST(V_pf_altqs_active);
		while ((altq != NULL) && (nr < pa->nr)) {
			altq = TAILQ_NEXT(altq, entries);
			nr++;
		}
		if (altq == NULL) {
			error = EBUSY;
			break;
		}
		bcopy(altq, &pa->altq, sizeof(struct pf_altq));
		break;
	}

	case DIOCCHANGEALTQ:
		/* CHANGEALTQ not supported yet! */
		error = ENODEV;
		break;

	case DIOCGETQSTATS: {
		struct pfioc_qstats	*pq = (struct pfioc_qstats *)addr;
		struct pf_altq		*altq;
		u_int32_t		 nr;
		int			 nbytes;

		if (pq->ticket != V_ticket_altqs_active) {
			error = EBUSY;
			break;
		}
		nbytes = pq->nbytes;
		nr = 0;
		altq = TAILQ_FIRST(V_pf_altqs_active);
		while ((altq != NULL) && (nr < pq->nr)) {
			altq = TAILQ_NEXT(altq, entries);
			nr++;
		}
		if (altq == NULL) {
			error = EBUSY;
			break;
		}

		if ((altq->local_flags & PFALTQ_FLAG_IF_REMOVED) != 0) {
			error = ENXIO;
			break;
		}
		PF_UNLOCK();
		error = altq_getqstats(altq, pq->buf, &nbytes);
		PF_LOCK();
		if (error == 0) {
			pq->scheduler = altq->scheduler;
			pq->nbytes = nbytes;
		}
		break;
	}
#endif /* ALTQ */

	case DIOCBEGINADDRS: {
		struct pfioc_pooladdr	*pp = (struct pfioc_pooladdr *)addr;

		pf_empty_pool(&V_pf_pabuf);
		pp->ticket = ++V_ticket_pabuf;
		break;
	}

	case DIOCADDADDR: {
		struct pfioc_pooladdr	*pp = (struct pfioc_pooladdr *)addr;

		if (pp->ticket != V_ticket_pabuf) {
			error = EBUSY;
			break;
		}
#ifndef INET
		if (pp->af == AF_INET) {
			error = EAFNOSUPPORT;
			break;
		}
#endif /* INET */
#ifndef INET6
		if (pp->af == AF_INET6) {
			error = EAFNOSUPPORT;
			break;
		}
#endif /* INET6 */
		if (pp->addr.addr.type != PF_ADDR_ADDRMASK &&
		    pp->addr.addr.type != PF_ADDR_DYNIFTL &&
		    pp->addr.addr.type != PF_ADDR_TABLE) {
			error = EINVAL;
			break;
		}
		pa = uma_zalloc(V_pf_pooladdr_pl, M_NOWAIT);
		if (pa == NULL) {
			error = ENOMEM;
			break;
		}
		bcopy(&pp->addr, pa, sizeof(struct pf_pooladdr));
		if (pa->ifname[0]) {
			pa->kif = pfi_kif_get(pa->ifname);
			if (pa->kif == NULL) {
				uma_zfree(V_pf_pooladdr_pl, pa);
				error = EINVAL;
				break;
			}
			pfi_kif_ref(pa->kif, PFI_KIF_REF_RULE);
		}
		if (pfi_dynaddr_setup(&pa->addr, pp->af)) {
			pfi_dynaddr_remove(&pa->addr);
			pfi_kif_unref(pa->kif, PFI_KIF_REF_RULE);
			uma_zfree(V_pf_pooladdr_pl, pa);
			error = EINVAL;
			break;
		}
		TAILQ_INSERT_TAIL(&V_pf_pabuf, pa, entries);
		break;
	}

	case DIOCGETADDRS: {
		struct pfioc_pooladdr	*pp = (struct pfioc_pooladdr *)addr;

		pp->nr = 0;
		pool = pf_get_pool(pp->anchor, pp->ticket, pp->r_action,
		    pp->r_num, 0, 1, 0);
		if (pool == NULL) {
			error = EBUSY;
			break;
		}
		TAILQ_FOREACH(pa, &pool->list, entries)
			pp->nr++;
		break;
	}

	case DIOCGETADDR: {
		struct pfioc_pooladdr	*pp = (struct pfioc_pooladdr *)addr;
		u_int32_t		 nr = 0;

		pool = pf_get_pool(pp->anchor, pp->ticket, pp->r_action,
		    pp->r_num, 0, 1, 1);
		if (pool == NULL) {
			error = EBUSY;
			break;
		}
		pa = TAILQ_FIRST(&pool->list);
		while ((pa != NULL) && (nr < pp->nr)) {
			pa = TAILQ_NEXT(pa, entries);
			nr++;
		}
		if (pa == NULL) {
			error = EBUSY;
			break;
		}
		bcopy(pa, &pp->addr, sizeof(struct pf_pooladdr));
		pf_addr_copyout(&pp->addr.addr);
		break;
	}

	case DIOCCHANGEADDR: {
		struct pfioc_pooladdr	*pca = (struct pfioc_pooladdr *)addr;
		struct pf_pooladdr	*oldpa = NULL, *newpa = NULL;
		struct pf_ruleset	*ruleset;

		if (pca->action < PF_CHANGE_ADD_HEAD ||
		    pca->action > PF_CHANGE_REMOVE) {
			error = EINVAL;
			break;
		}
		if (pca->addr.addr.type != PF_ADDR_ADDRMASK &&
		    pca->addr.addr.type != PF_ADDR_DYNIFTL &&
		    pca->addr.addr.type != PF_ADDR_TABLE) {
			error = EINVAL;
			break;
		}

		ruleset = pf_find_ruleset(pca->anchor);
		if (ruleset == NULL) {
			error = EBUSY;
			break;
		}
		pool = pf_get_pool(pca->anchor, pca->ticket, pca->r_action,
		    pca->r_num, pca->r_last, 1, 1);
		if (pool == NULL) {
			error = EBUSY;
			break;
		}
		if (pca->action != PF_CHANGE_REMOVE) {
			newpa = uma_zalloc(V_pf_pooladdr_pl,
			    M_NOWAIT);
			if (newpa == NULL) {
				error = ENOMEM;
				break;
			}
			bcopy(&pca->addr, newpa, sizeof(struct pf_pooladdr));
#ifndef INET
			if (pca->af == AF_INET) {
				uma_zfree(V_pf_pooladdr_pl, newpa);
				error = EAFNOSUPPORT;
				break;
			}
#endif /* INET */
#ifndef INET6
			if (pca->af == AF_INET6) {
				uma_zfree(V_pf_pooladdr_pl, newpa);
				error = EAFNOSUPPORT;
				break;
			}
#endif /* INET6 */
			if (newpa->ifname[0]) {
				newpa->kif = pfi_kif_get(newpa->ifname);
				if (newpa->kif == NULL) {
					uma_zfree(V_pf_pooladdr_pl, newpa);
					error = EINVAL;
					break;
				}
				pfi_kif_ref(newpa->kif, PFI_KIF_REF_RULE);
			} else
				newpa->kif = NULL;
			if (pfi_dynaddr_setup(&newpa->addr, pca->af) ||
			    pf_tbladdr_setup(ruleset, &newpa->addr)) {
				pfi_dynaddr_remove(&newpa->addr);
				pfi_kif_unref(newpa->kif, PFI_KIF_REF_RULE);
				uma_zfree(V_pf_pooladdr_pl, newpa);
				error = EINVAL;
				break;
			}
		}

		if (pca->action == PF_CHANGE_ADD_HEAD)
			oldpa = TAILQ_FIRST(&pool->list);
		else if (pca->action == PF_CHANGE_ADD_TAIL)
			oldpa = TAILQ_LAST(&pool->list, pf_palist);
		else {
			int	i = 0;

			oldpa = TAILQ_FIRST(&pool->list);
			while ((oldpa != NULL) && (i < pca->nr)) {
				oldpa = TAILQ_NEXT(oldpa, entries);
				i++;
			}
			if (oldpa == NULL) {
				error = EINVAL;
				break;
			}
		}

		if (pca->action == PF_CHANGE_REMOVE) {
			TAILQ_REMOVE(&pool->list, oldpa, entries);
			pfi_dynaddr_remove(&oldpa->addr);
			pf_tbladdr_remove(&oldpa->addr);
			pfi_kif_unref(oldpa->kif, PFI_KIF_REF_RULE);
			uma_zfree(V_pf_pooladdr_pl, oldpa);
		} else {
			if (oldpa == NULL)
				TAILQ_INSERT_TAIL(&pool->list, newpa, entries);
			else if (pca->action == PF_CHANGE_ADD_HEAD ||
			    pca->action == PF_CHANGE_ADD_BEFORE)
				TAILQ_INSERT_BEFORE(oldpa, newpa, entries);
			else
				TAILQ_INSERT_AFTER(&pool->list, oldpa,
				    newpa, entries);
		}

		pool->cur = TAILQ_FIRST(&pool->list);
		PF_ACPY(&pool->counter, &pool->cur->addr.v.a.addr,
		    pca->af);
		break;
	}

	case DIOCGETRULESETS: {
		struct pfioc_ruleset	*pr = (struct pfioc_ruleset *)addr;
		struct pf_ruleset	*ruleset;
		struct pf_anchor	*anchor;

		pr->path[sizeof(pr->path) - 1] = 0;
		if ((ruleset = pf_find_ruleset(pr->path)) == NULL) {
			error = EINVAL;
			break;
		}
		pr->nr = 0;
		if (ruleset->anchor == NULL) {
			/* XXX kludge for pf_main_ruleset */
			RB_FOREACH(anchor, pf_anchor_global, &V_pf_anchors)
				if (anchor->parent == NULL)
					pr->nr++;
		} else {
			RB_FOREACH(anchor, pf_anchor_node,
			    &ruleset->anchor->children)
				pr->nr++;
		}
		break;
	}

	case DIOCGETRULESET: {
		struct pfioc_ruleset	*pr = (struct pfioc_ruleset *)addr;
		struct pf_ruleset	*ruleset;
		struct pf_anchor	*anchor;
		u_int32_t		 nr = 0;

		pr->path[sizeof(pr->path) - 1] = 0;
		if ((ruleset = pf_find_ruleset(pr->path)) == NULL) {
			error = EINVAL;
			break;
		}
		pr->name[0] = 0;
		if (ruleset->anchor == NULL) {
			/* XXX kludge for pf_main_ruleset */
			RB_FOREACH(anchor, pf_anchor_global, &V_pf_anchors)
				if (anchor->parent == NULL && nr++ == pr->nr) {
					strlcpy(pr->name, anchor->name,
					    sizeof(pr->name));
					break;
				}
		} else {
			RB_FOREACH(anchor, pf_anchor_node,
			    &ruleset->anchor->children)
				if (nr++ == pr->nr) {
					strlcpy(pr->name, anchor->name,
					    sizeof(pr->name));
					break;
				}
		}
		if (!pr->name[0])
			error = EBUSY;
		break;
	}

	case DIOCRCLRTABLES: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != 0) {
			error = ENODEV;
			break;
		}
		error = pfr_clr_tables(&io->pfrio_table, &io->pfrio_ndel,
		    io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRADDTABLES: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_table)) {
			error = ENODEV;
			break;
		}
		error = pfr_add_tables(io->pfrio_buffer, io->pfrio_size,
		    &io->pfrio_nadd, io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRDELTABLES: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_table)) {
			error = ENODEV;
			break;
		}
		error = pfr_del_tables(io->pfrio_buffer, io->pfrio_size,
		    &io->pfrio_ndel, io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRGETTABLES: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_table)) {
			error = ENODEV;
			break;
		}
		error = pfr_get_tables(&io->pfrio_table, io->pfrio_buffer,
		    &io->pfrio_size, io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRGETTSTATS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_tstats)) {
			error = ENODEV;
			break;
		}
		error = pfr_get_tstats(&io->pfrio_table, io->pfrio_buffer,
		    &io->pfrio_size, io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRCLRTSTATS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_table)) {
			error = ENODEV;
			break;
		}
		error = pfr_clr_tstats(io->pfrio_buffer, io->pfrio_size,
		    &io->pfrio_nzero, io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRSETTFLAGS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_table)) {
			error = ENODEV;
			break;
		}
		error = pfr_set_tflags(io->pfrio_buffer, io->pfrio_size,
		    io->pfrio_setflag, io->pfrio_clrflag, &io->pfrio_nchange,
		    &io->pfrio_ndel, io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRCLRADDRS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != 0) {
			error = ENODEV;
			break;
		}
		error = pfr_clr_addrs(&io->pfrio_table, &io->pfrio_ndel,
		    io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRADDADDRS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_addr)) {
			error = ENODEV;
			break;
		}
		error = pfr_add_addrs(&io->pfrio_table, io->pfrio_buffer,
		    io->pfrio_size, &io->pfrio_nadd, io->pfrio_flags |
		    PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRDELADDRS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_addr)) {
			error = ENODEV;
			break;
		}
		error = pfr_del_addrs(&io->pfrio_table, io->pfrio_buffer,
		    io->pfrio_size, &io->pfrio_ndel, io->pfrio_flags |
		    PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRSETADDRS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_addr)) {
			error = ENODEV;
			break;
		}
		error = pfr_set_addrs(&io->pfrio_table, io->pfrio_buffer,
		    io->pfrio_size, &io->pfrio_size2, &io->pfrio_nadd,
		    &io->pfrio_ndel, &io->pfrio_nchange, io->pfrio_flags |
		    PFR_FLAG_USERIOCTL, 0);
		break;
	}

	case DIOCRGETADDRS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_addr)) {
			error = ENODEV;
			break;
		}
		error = pfr_get_addrs(&io->pfrio_table, io->pfrio_buffer,
		    &io->pfrio_size, io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRGETASTATS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_astats)) {
			error = ENODEV;
			break;
		}
		error = pfr_get_astats(&io->pfrio_table, io->pfrio_buffer,
		    &io->pfrio_size, io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRCLRASTATS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_addr)) {
			error = ENODEV;
			break;
		}
		error = pfr_clr_astats(&io->pfrio_table, io->pfrio_buffer,
		    io->pfrio_size, &io->pfrio_nzero, io->pfrio_flags |
		    PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRTSTADDRS: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_addr)) {
			error = ENODEV;
			break;
		}
		error = pfr_tst_addrs(&io->pfrio_table, io->pfrio_buffer,
		    io->pfrio_size, &io->pfrio_nmatch, io->pfrio_flags |
		    PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCRINADEFINE: {
		struct pfioc_table *io = (struct pfioc_table *)addr;

		if (io->pfrio_esize != sizeof(struct pfr_addr)) {
			error = ENODEV;
			break;
		}
		error = pfr_ina_define(&io->pfrio_table, io->pfrio_buffer,
		    io->pfrio_size, &io->pfrio_nadd, &io->pfrio_naddr,
		    io->pfrio_ticket, io->pfrio_flags | PFR_FLAG_USERIOCTL);
		break;
	}

	case DIOCOSFPADD: {
		struct pf_osfp_ioctl *io = (struct pf_osfp_ioctl *)addr;
		error = pf_osfp_add(io);
		break;
	}

	case DIOCOSFPGET: {
		struct pf_osfp_ioctl *io = (struct pf_osfp_ioctl *)addr;
		error = pf_osfp_get(io);
		break;
	}

	case DIOCXBEGIN: {
		struct pfioc_trans	*io = (struct pfioc_trans *)addr;
		struct pfioc_trans_e	*ioe;
		struct pfr_table	*table;
		int			 i;

		if (io->esize != sizeof(*ioe)) {
			error = ENODEV;
			goto fail;
		}
		PF_UNLOCK();
		ioe = malloc(sizeof(*ioe), M_TEMP, M_WAITOK);
		table = malloc(sizeof(*table), M_TEMP, M_WAITOK);
		PF_LOCK();
		for (i = 0; i < io->size; i++) {
		PF_COPYIN(io->array+i, ioe, sizeof(*ioe), error);
		if (error) {
				free(table, M_TEMP);
				free(ioe, M_TEMP);
				error = EFAULT;
				goto fail;
			}
			switch (ioe->rs_num) {
#ifdef ALTQ
			case PF_RULESET_ALTQ:
				if (ioe->anchor[0]) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					error = EINVAL;
					goto fail;
				}
				if ((error = pf_begin_altq(&ioe->ticket))) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					goto fail;
				}
				break;
#endif /* ALTQ */
			case PF_RULESET_TABLE:
				bzero(table, sizeof(*table));
				strlcpy(table->pfrt_anchor, ioe->anchor,
				    sizeof(table->pfrt_anchor));
				if ((error = pfr_ina_begin(table,
				    &ioe->ticket, NULL, 0))) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					goto fail;
				}
				break;
			default:
				if ((error = pf_begin_rules(&ioe->ticket,
				    ioe->rs_num, ioe->anchor))) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					goto fail;
				}
				break;
			}
			PF_COPYOUT(ioe, io->array+i, sizeof(io->array[i]),
			    error);
			if (error) {
				free(table, M_TEMP);
				free(ioe, M_TEMP);
				error = EFAULT;
				goto fail;
			}
		}
		free(table, M_TEMP);
		free(ioe, M_TEMP);
		break;
	}

	case DIOCXROLLBACK: {
		struct pfioc_trans	*io = (struct pfioc_trans *)addr;
		struct pfioc_trans_e	*ioe;
		struct pfr_table	*table;
		int			 i;

		if (io->esize != sizeof(*ioe)) {
			error = ENODEV;
			goto fail;
		}
		PF_UNLOCK();
		ioe = malloc(sizeof(*ioe), M_TEMP, M_WAITOK);
		table = malloc(sizeof(*table), M_TEMP, M_WAITOK);
		PF_LOCK();
		for (i = 0; i < io->size; i++) {
			PF_COPYIN(io->array+i, ioe, sizeof(*ioe), error);
			if (error) {
				free(table, M_TEMP);
				free(ioe, M_TEMP);
				error = EFAULT;
				goto fail;
			}
			switch (ioe->rs_num) {
#ifdef ALTQ
			case PF_RULESET_ALTQ:
				if (ioe->anchor[0]) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					error = EINVAL;
					goto fail;
				}
				if ((error = pf_rollback_altq(ioe->ticket))) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					goto fail; /* really bad */
				}
				break;
#endif /* ALTQ */
			case PF_RULESET_TABLE:
				bzero(table, sizeof(*table));
				strlcpy(table->pfrt_anchor, ioe->anchor,
				    sizeof(table->pfrt_anchor));
				if ((error = pfr_ina_rollback(table,
				    ioe->ticket, NULL, 0))) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					goto fail; /* really bad */
				}
				break;
			default:
				if ((error = pf_rollback_rules(ioe->ticket,
				    ioe->rs_num, ioe->anchor))) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					goto fail; /* really bad */
				}
				break;
			}
		}
		free(table, M_TEMP);
		free(ioe, M_TEMP);
		break;
	}

	case DIOCXCOMMIT: {
		struct pfioc_trans	*io = (struct pfioc_trans *)addr;
		struct pfioc_trans_e	*ioe;
		struct pfr_table	*table;
		struct pf_ruleset	*rs;
		int			 i;

		if (io->esize != sizeof(*ioe)) {
			error = ENODEV;
			goto fail;
		}
		PF_UNLOCK();
		ioe = malloc(sizeof(*ioe), M_TEMP, M_WAITOK);
		table = malloc(sizeof(*table), M_TEMP, M_WAITOK);
		PF_LOCK();
		/* first makes sure everything will succeed */
		for (i = 0; i < io->size; i++) {
			PF_COPYIN(io->array+i, ioe, sizeof(*ioe), error);
			if (error) {
				free(table, M_TEMP);
				free(ioe, M_TEMP);
				error = EFAULT;
				goto fail;
			}
			switch (ioe->rs_num) {
#ifdef ALTQ
			case PF_RULESET_ALTQ:
				if (ioe->anchor[0]) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					error = EINVAL;
					goto fail;
				}
				if (!V_altqs_inactive_open || ioe->ticket !=
				    V_ticket_altqs_inactive) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					error = EBUSY;
					goto fail;
				}
				break;
#endif /* ALTQ */
			case PF_RULESET_TABLE:
				rs = pf_find_ruleset(ioe->anchor);
				if (rs == NULL || !rs->topen || ioe->ticket !=
				    rs->tticket) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					error = EBUSY;
					goto fail;
				}
				break;
			default:
				if (ioe->rs_num < 0 || ioe->rs_num >=
				    PF_RULESET_MAX) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					error = EINVAL;
					goto fail;
				}
				rs = pf_find_ruleset(ioe->anchor);
				if (rs == NULL ||
				    !rs->rules[ioe->rs_num].inactive.open ||
				    rs->rules[ioe->rs_num].inactive.ticket !=
				    ioe->ticket) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					error = EBUSY;
					goto fail;
				}
				break;
			}
		}
		/* now do the commit - no errors should happen here */
		for (i = 0; i < io->size; i++) {
			PF_COPYIN(io->array+i, ioe, sizeof(*ioe), error);
			if (error) {
				free(table, M_TEMP);
				free(ioe, M_TEMP);
				error = EFAULT;
				goto fail;
			}
			switch (ioe->rs_num) {
#ifdef ALTQ
			case PF_RULESET_ALTQ:
				if ((error = pf_commit_altq(ioe->ticket))) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					goto fail; /* really bad */
				}
				break;
#endif /* ALTQ */
			case PF_RULESET_TABLE:
				bzero(table, sizeof(*table));
				strlcpy(table->pfrt_anchor, ioe->anchor,
				    sizeof(table->pfrt_anchor));
				if ((error = pfr_ina_commit(table, ioe->ticket,
				    NULL, NULL, 0))) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					goto fail; /* really bad */
				}
				break;
			default:
				if ((error = pf_commit_rules(ioe->ticket,
				    ioe->rs_num, ioe->anchor))) {
					free(table, M_TEMP);
					free(ioe, M_TEMP);
					goto fail; /* really bad */
				}
				break;
			}
		}
		free(table, M_TEMP);
		free(ioe, M_TEMP);
		break;
	}

	case DIOCGETSRCNODES: {
		struct pfioc_src_nodes	*psn = (struct pfioc_src_nodes *)addr;
		struct pf_src_node	*n, *p, *pstore;
		u_int32_t		 nr = 0;
		int			 space = psn->psn_len;

		if (space == 0) {
			RB_FOREACH(n, pf_src_tree, &V_tree_src_tracking)
				nr++;
			psn->psn_len = sizeof(struct pf_src_node) * nr;
			break;
		}

		PF_UNLOCK();
		pstore = malloc(sizeof(*pstore), M_TEMP, M_WAITOK);
		PF_LOCK();
		p = psn->psn_src_nodes;
		RB_FOREACH(n, pf_src_tree, &V_tree_src_tracking) {
			int	secs = time_second, diff;

			if ((nr + 1) * sizeof(*p) > (unsigned)psn->psn_len)
				break;

			bcopy(n, pstore, sizeof(*pstore));
			if (n->rule.ptr != NULL)
				pstore->rule.nr = n->rule.ptr->nr;
			pstore->creation = secs - pstore->creation;
			if (pstore->expire > secs)
				pstore->expire -= secs;
			else
				pstore->expire = 0;

			/* adjust the connection rate estimate */
			diff = secs - n->conn_rate.last;
			if (diff >= n->conn_rate.seconds)
				pstore->conn_rate.count = 0;
			else
				pstore->conn_rate.count -=
				    n->conn_rate.count * diff /
				    n->conn_rate.seconds;

			PF_COPYOUT(pstore, p, sizeof(*p), error);
			if (error) {
				free(pstore, M_TEMP);
				goto fail;
			}
			p++;
			nr++;
		}
		psn->psn_len = sizeof(struct pf_src_node) * nr;

		free(pstore, M_TEMP);
		break;
	}

	case DIOCCLRSRCNODES: {
		struct pf_src_node	*n;
		struct pf_state		*state;

		RB_FOREACH(state, pf_state_tree_id, &V_tree_id) {
			state->src_node = NULL;
			state->nat_src_node = NULL;
		}
		RB_FOREACH(n, pf_src_tree, &V_tree_src_tracking) {
			n->expire = 1;
			n->states = 0;
		}
		pf_purge_expired_src_nodes(1);
		V_pf_status.src_nodes = 0;
		break;
	}

	case DIOCKILLSRCNODES: {
		struct pf_src_node	*sn;
		struct pf_state		*s;
		struct pfioc_src_node_kill *psnk =
		    (struct pfioc_src_node_kill *)addr;
		u_int			killed = 0;

		RB_FOREACH(sn, pf_src_tree, &V_tree_src_tracking) {
			if (PF_MATCHA(psnk->psnk_src.neg,
				&psnk->psnk_src.addr.v.a.addr,
				&psnk->psnk_src.addr.v.a.mask,
				&sn->addr, sn->af) &&
			    PF_MATCHA(psnk->psnk_dst.neg,
				&psnk->psnk_dst.addr.v.a.addr,
				&psnk->psnk_dst.addr.v.a.mask,
				&sn->raddr, sn->af)) {
				/* Handle state to src_node linkage */
				if (sn->states != 0) {
					RB_FOREACH(s, pf_state_tree_id,
					    &V_tree_id) {
						if (s->src_node == sn)
							s->src_node = NULL;
						if (s->nat_src_node == sn)
							s->nat_src_node = NULL;
					}
					sn->states = 0;
				}
				sn->expire = 1;
				killed++;
			}
		}

		if (killed > 0)
			pf_purge_expired_src_nodes(1);

		psnk->psnk_killed = killed;
		break;
	}

	case DIOCSETHOSTID: {
		u_int32_t	*hostid = (u_int32_t *)addr;

		if (*hostid == 0)
			V_pf_status.hostid = arc4random();
		else
			V_pf_status.hostid = *hostid;
		break;
	}

	case DIOCOSFPFLUSH:
		pf_osfp_flush();
		break;

	case DIOCIGETIFACES: {
		struct pfioc_iface *io = (struct pfioc_iface *)addr;

		if (io->pfiio_esize != sizeof(struct pfi_kif)) {
			error = ENODEV;
			break;
		}
		error = pfi_get_ifaces(io->pfiio_name, io->pfiio_buffer,
		    &io->pfiio_size);
		break;
	}

	case DIOCSETIFFLAG: {
		struct pfioc_iface *io = (struct pfioc_iface *)addr;

		error = pfi_set_flags(io->pfiio_name, io->pfiio_flags);
		break;
	}

	case DIOCCLRIFFLAG: {
		struct pfioc_iface *io = (struct pfioc_iface *)addr;

		error = pfi_clear_flags(io->pfiio_name, io->pfiio_flags);
		break;
	}

	default:
		error = ENODEV;
		break;
	}
fail:
	PF_UNLOCK();

	if (flags & FWRITE)
		sx_xunlock(&V_pf_consistency_lock);
	else
		sx_sunlock(&V_pf_consistency_lock);

	CURVNET_RESTORE();

	return (error);
}

void
pfsync_state_export(struct pfsync_state *sp, struct pf_state *st)
{
	bzero(sp, sizeof(struct pfsync_state));

	/* copy from state key */
	sp->key[PF_SK_WIRE].addr[0] = st->key[PF_SK_WIRE]->addr[0];
	sp->key[PF_SK_WIRE].addr[1] = st->key[PF_SK_WIRE]->addr[1];
	sp->key[PF_SK_WIRE].port[0] = st->key[PF_SK_WIRE]->port[0];
	sp->key[PF_SK_WIRE].port[1] = st->key[PF_SK_WIRE]->port[1];
	sp->key[PF_SK_STACK].addr[0] = st->key[PF_SK_STACK]->addr[0];
	sp->key[PF_SK_STACK].addr[1] = st->key[PF_SK_STACK]->addr[1];
	sp->key[PF_SK_STACK].port[0] = st->key[PF_SK_STACK]->port[0];
	sp->key[PF_SK_STACK].port[1] = st->key[PF_SK_STACK]->port[1];
	sp->proto = st->key[PF_SK_WIRE]->proto;
	sp->af = st->key[PF_SK_WIRE]->af;

	/* copy from state */
	strlcpy(sp->ifname, st->kif->pfik_name, sizeof(sp->ifname));
	bcopy(&st->rt_addr, &sp->rt_addr, sizeof(sp->rt_addr));
	sp->creation = htonl(time_second - st->creation);
	sp->expire = pf_state_expires(st);
	if (sp->expire <= time_second)
		sp->expire = htonl(0);
	else
		sp->expire = htonl(sp->expire - time_second);

	sp->direction = st->direction;
	sp->log = st->log;
	sp->timeout = st->timeout;
	sp->state_flags = st->state_flags;
	if (st->src_node)
		sp->sync_flags |= PFSYNC_FLAG_SRCNODE;
	if (st->nat_src_node)
		sp->sync_flags |= PFSYNC_FLAG_NATSRCNODE;

	bcopy(&st->id, &sp->id, sizeof(sp->id));
	sp->creatorid = st->creatorid;
	pf_state_peer_hton(&st->src, &sp->src);
	pf_state_peer_hton(&st->dst, &sp->dst);

	if (st->rule.ptr == NULL)
		sp->rule = htonl(-1);
	else
		sp->rule = htonl(st->rule.ptr->nr);
	if (st->anchor.ptr == NULL)
		sp->anchor = htonl(-1);
	else
		sp->anchor = htonl(st->anchor.ptr->nr);
	if (st->nat_rule.ptr == NULL)
		sp->nat_rule = htonl(-1);
	else
		sp->nat_rule = htonl(st->nat_rule.ptr->nr);

	pf_state_counter_hton(st->packets[0], sp->packets[0]);
	pf_state_counter_hton(st->packets[1], sp->packets[1]);
	pf_state_counter_hton(st->bytes[0], sp->bytes[0]);
	pf_state_counter_hton(st->bytes[1], sp->bytes[1]);

}

/*
 * XXX - Check for version missmatch!!!
 */
static void
pf_clear_states(void)
{
	struct pf_state	*state;
 
	RB_FOREACH(state, pf_state_tree_id, &V_tree_id) {
		state->timeout = PFTM_PURGE;
		/* don't send out individual delete messages */
		state->sync_state = PFSTATE_NOSYNC;
		pf_unlink_state(state);
	}
 
#if 0 /* NPFSYNC */
/*
 * XXX This is called on module unload, we do not want to sync that over? */
 */
	pfsync_clear_states(V_pf_status.hostid, psk->psk_ifname);
#endif
}

static int
pf_clear_tables(void)
{
	struct pfioc_table io;
	int error;

	bzero(&io, sizeof(io));

	error = pfr_clr_tables(&io.pfrio_table, &io.pfrio_ndel,
	    io.pfrio_flags);

	return (error);
}

static void
pf_clear_srcnodes(void)
{
	struct pf_src_node	*n;
	struct pf_state		*state;

	RB_FOREACH(state, pf_state_tree_id, &V_tree_id) {
		state->src_node = NULL;
		state->nat_src_node = NULL;
	}
	RB_FOREACH(n, pf_src_tree, &V_tree_src_tracking) {
		n->expire = 1;
		n->states = 0;
	}
}
/*
 * XXX - Check for version missmatch!!!
 */

/*
 * Duplicate pfctl -Fa operation to get rid of as much as we can.
 */
static int
shutdown_pf(void)
{
	int error = 0;
	u_int32_t t[5];
	char nn = '\0';
 
	V_pf_status.running = 0;
	do {
		if ((error = pf_begin_rules(&t[0], PF_RULESET_SCRUB, &nn))
		    != 0) {
			DPFPRINTF(PF_DEBUG_MISC, ("shutdown_pf: SCRUB\n"));
			break;
		}
		if ((error = pf_begin_rules(&t[1], PF_RULESET_FILTER, &nn))
		    != 0) {
			DPFPRINTF(PF_DEBUG_MISC, ("shutdown_pf: FILTER\n"));
			break;          /* XXX: rollback? */
		}
		if ((error = pf_begin_rules(&t[2], PF_RULESET_NAT, &nn))
		    != 0) {
			DPFPRINTF(PF_DEBUG_MISC, ("shutdown_pf: NAT\n"));
			break;          /* XXX: rollback? */
		}
		if ((error = pf_begin_rules(&t[3], PF_RULESET_BINAT, &nn))
		    != 0) {
			DPFPRINTF(PF_DEBUG_MISC, ("shutdown_pf: BINAT\n"));
			break;          /* XXX: rollback? */
		}
		if ((error = pf_begin_rules(&t[4], PF_RULESET_RDR, &nn))
		    != 0) {
			DPFPRINTF(PF_DEBUG_MISC, ("shutdown_pf: RDR\n"));
			break;          /* XXX: rollback? */
		}

		/* XXX: these should always succeed here */
		pf_commit_rules(t[0], PF_RULESET_SCRUB, &nn);
		pf_commit_rules(t[1], PF_RULESET_FILTER, &nn);
		pf_commit_rules(t[2], PF_RULESET_NAT, &nn);
		pf_commit_rules(t[3], PF_RULESET_BINAT, &nn);
		pf_commit_rules(t[4], PF_RULESET_RDR, &nn);

		if ((error = pf_clear_tables()) != 0)
			break;

	#ifdef ALTQ
		if ((error = pf_begin_altq(&t[0])) != 0) {
			DPFPRINTF(PF_DEBUG_MISC, ("shutdown_pf: ALTQ\n"));
			break;
		}
		pf_commit_altq(t[0]);
	#endif

		pf_clear_states();

		pf_clear_srcnodes();

		/* status does not use malloced mem so no need to cleanup */
		/* fingerprints and interfaces have thier own cleanup code */
	} while(0);

	return (error);
}

#ifdef INET
static int
pf_check_in(void *arg, struct mbuf **m, struct ifnet *ifp, int dir,
    struct inpcb *inp)
{
	/*
	 * XXX Wed Jul 9 22:03:16 2003 UTC
	 * OpenBSD has changed its byte ordering convention on ip_len/ip_off
	 * in network stack. OpenBSD's network stack have converted
	 * ip_len/ip_off to host byte order frist as FreeBSD.
	 * Now this is not true anymore , so we should convert back to network
	 * byte order. 
	 */
	struct ip *h = NULL;
	int chk;

	if ((*m)->m_pkthdr.len >= (int)sizeof(struct ip)) {
		/* if m_pkthdr.len is less than ip header, pf will handle. */
		h = mtod(*m, struct ip *);
		HTONS(h->ip_len);
		HTONS(h->ip_off);
	}
	CURVNET_SET(ifp->if_vnet);
	chk = pf_test(PF_IN, ifp, m, NULL, inp);
	CURVNET_RESTORE();
	if (chk && *m) {
		m_freem(*m);
		*m = NULL;
	}
	if (*m != NULL) {
		/* pf_test can change ip header location */
		h = mtod(*m, struct ip *);
		NTOHS(h->ip_len);
		NTOHS(h->ip_off);
	}
	return chk;
}

static int
pf_check_out(void *arg, struct mbuf **m, struct ifnet *ifp, int dir,
    struct inpcb *inp)
{
	/*
	 * XXX Wed Jul 9 22:03:16 2003 UTC
	 * OpenBSD has changed its byte ordering convention on ip_len/ip_off
	 * in network stack. OpenBSD's network stack have converted
	 * ip_len/ip_off to host byte order frist as FreeBSD.
	 * Now this is not true anymore , so we should convert back to network
	 * byte order. 
	 */
	struct ip *h = NULL;
	int chk;

	/* We need a proper CSUM befor we start (s. OpenBSD ip_output) */
	if ((*m)->m_pkthdr.csum_flags & CSUM_DELAY_DATA) {
		in_delayed_cksum(*m);
		(*m)->m_pkthdr.csum_flags &= ~CSUM_DELAY_DATA;
	}
	if ((*m)->m_pkthdr.len >= (int)sizeof(*h)) {
		/* if m_pkthdr.len is less than ip header, pf will handle. */
		h = mtod(*m, struct ip *);
		HTONS(h->ip_len);
		HTONS(h->ip_off);
	}
	CURVNET_SET(ifp->if_vnet);
	chk = pf_test(PF_OUT, ifp, m, NULL, inp);
	CURVNET_RESTORE();
	if (chk && *m) {
		m_freem(*m);
		*m = NULL;
	}
	if (*m != NULL) {
		/* pf_test can change ip header location */
		h = mtod(*m, struct ip *);
		NTOHS(h->ip_len);
		NTOHS(h->ip_off);
	}
	return chk;
}
#endif

#ifdef INET6
static int
pf_check6_in(void *arg, struct mbuf **m, struct ifnet *ifp, int dir,
    struct inpcb *inp)
{

	/*
	 * IPv6 is not affected by ip_len/ip_off byte order changes.
	 */
	int chk;

	/*
	 * In case of loopback traffic IPv6 uses the real interface in
	 * order to support scoped addresses. In order to support stateful
	 * filtering we have change this to lo0 as it is the case in IPv4.
	 */
	CURVNET_SET(ifp->if_vnet);
	chk = pf_test6(PF_IN, (*m)->m_flags & M_LOOP ? V_loif : ifp, m,
	    NULL, inp);
	CURVNET_RESTORE();
	if (chk && *m) {
		m_freem(*m);
		*m = NULL;
	}
	return chk;
}

static int
pf_check6_out(void *arg, struct mbuf **m, struct ifnet *ifp, int dir,
    struct inpcb *inp)
{
	/*
	 * IPv6 does not affected ip_len/ip_off byte order changes.
	 */
	int chk;

	/* We need a proper CSUM before we start (s. OpenBSD ip_output) */
	if ((*m)->m_pkthdr.csum_flags & CSUM_DELAY_DATA) {
#ifdef INET
		/* XXX-BZ copy&paste error from r126261? */
		in_delayed_cksum(*m);
#endif
		(*m)->m_pkthdr.csum_flags &= ~CSUM_DELAY_DATA;
	}
	CURVNET_SET(ifp->if_vnet);
	chk = pf_test6(PF_OUT, ifp, m, NULL, inp);
	CURVNET_RESTORE();
	if (chk && *m) {
		m_freem(*m);
		*m = NULL;
	}
	return chk;
}
#endif /* INET6 */

static int
hook_pf(void)
{
#ifdef INET
	struct pfil_head *pfh_inet;
#endif
#ifdef INET6
	struct pfil_head *pfh_inet6;
#endif

	PF_UNLOCK_ASSERT();

	if (V_pf_pfil_hooked)
		return (0); 

#ifdef INET
	pfh_inet = pfil_head_get(PFIL_TYPE_AF, AF_INET);
	if (pfh_inet == NULL)
		return (ESRCH); /* XXX */
	pfil_add_hook(pf_check_in, NULL, PFIL_IN | PFIL_WAITOK, pfh_inet);
	pfil_add_hook(pf_check_out, NULL, PFIL_OUT | PFIL_WAITOK, pfh_inet);
#endif
#ifdef INET6
	pfh_inet6 = pfil_head_get(PFIL_TYPE_AF, AF_INET6);
	if (pfh_inet6 == NULL) {
#ifdef INET
		pfil_remove_hook(pf_check_in, NULL, PFIL_IN | PFIL_WAITOK,
		    pfh_inet);
		pfil_remove_hook(pf_check_out, NULL, PFIL_OUT | PFIL_WAITOK,
		    pfh_inet);
#endif
		return (ESRCH); /* XXX */
	}
	pfil_add_hook(pf_check6_in, NULL, PFIL_IN | PFIL_WAITOK, pfh_inet6);
	pfil_add_hook(pf_check6_out, NULL, PFIL_OUT | PFIL_WAITOK, pfh_inet6);
#endif

	V_pf_pfil_hooked = 1;
	return (0);
}

static int
dehook_pf(void)
{
#ifdef INET
	struct pfil_head *pfh_inet;
#endif
#ifdef INET6
	struct pfil_head *pfh_inet6;
#endif

	PF_UNLOCK_ASSERT();

	if (V_pf_pfil_hooked == 0)
		return (0);

#ifdef INET
	pfh_inet = pfil_head_get(PFIL_TYPE_AF, AF_INET);
	if (pfh_inet == NULL)
		return (ESRCH); /* XXX */
	pfil_remove_hook(pf_check_in, NULL, PFIL_IN | PFIL_WAITOK,
	    pfh_inet);
	pfil_remove_hook(pf_check_out, NULL, PFIL_OUT | PFIL_WAITOK,
	    pfh_inet);
#endif
#ifdef INET6
	pfh_inet6 = pfil_head_get(PFIL_TYPE_AF, AF_INET6);
	if (pfh_inet6 == NULL)
		return (ESRCH); /* XXX */
	pfil_remove_hook(pf_check6_in, NULL, PFIL_IN | PFIL_WAITOK,
	    pfh_inet6);
	pfil_remove_hook(pf_check6_out, NULL, PFIL_OUT | PFIL_WAITOK,
	    pfh_inet6);
#endif

	V_pf_pfil_hooked = 0;
	return (0);
}

/*
 * Must be called whenever any addressing information such as
 * address, port, protocol has changed.
 */
static void
pf_pkt_addr_changed(struct mbuf *m)
{
	struct pf_mtag	*pf_tag;

	if ((pf_tag = pf_find_mtag(m)) != NULL)
		pf_tag->statekey = NULL;
}

static int
pf_load(void)
{
	int error;

	VNET_ITERATOR_DECL(vnet_iter);

	VNET_LIST_RLOCK();
	VNET_FOREACH(vnet_iter) {
		CURVNET_SET(vnet_iter);
		V_pf_pfil_hooked = 0;
		V_pf_end_threads = 0;
		V_debug_pfugidhack = 0;
		TAILQ_INIT(&V_pf_tags);
		TAILQ_INIT(&V_pf_qids);
		CURVNET_RESTORE();
	}
	VNET_LIST_RUNLOCK();

	init_pf_mutex();
	pf_dev = make_dev(&pf_cdevsw, 0, 0, 0, 0600, PF_NAME);
	if ((error = pfattach()) != 0)
		return (error);

	return (0);
}

static int
pf_unload(void)
{
	int error = 0;

	PF_LOCK();
	V_pf_status.running = 0;
	PF_UNLOCK();
	m_addr_chg_pf_p = NULL;
	error = dehook_pf();
	if (error) {
		/*
		 * Should not happen!
		 * XXX Due to error code ESRCH, kldunload will show
		 * a message like 'No such process'.
		 */
		printf("%s : pfil unregisteration fail\n", __FUNCTION__);
		return error;
	}
	PF_LOCK();
	shutdown_pf();
	V_pf_end_threads = 1;
	while (V_pf_end_threads < 2) {
		wakeup_one(pf_purge_thread);
		msleep(pf_purge_thread, &pf_mtx, 0, "pftmo", hz);
	}
	pf_normalize_cleanup();
	pfi_cleanup();
	pf_osfp_flush();
	pf_osfp_cleanup();
	cleanup_pf_zone();
	PF_UNLOCK();
	destroy_dev(pf_dev);
	destroy_pf_mutex();

	return (error);
}

static int
pf_modevent(module_t mod, int type, void *data)
{
	int error = 0;

	switch(type) {
	case MOD_LOAD:
		error = pf_load();
		break;
	case MOD_QUIESCE:
		/*
		 * Module should not be unloaded due to race conditions.
		 */
		error = EPERM;
		break;
	case MOD_UNLOAD:
		error = pf_unload();
		break;
	default:
		error = EINVAL;
		break;
	}

	return (error);
}
 
static moduledata_t pf_mod = {
	"pf",
	pf_modevent,
	0
};

DECLARE_MODULE(pf, pf_mod, SI_SUB_PSEUDO, SI_ORDER_FIRST);
MODULE_VERSION(pf, PF_MODVER);
