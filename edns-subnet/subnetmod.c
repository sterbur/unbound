/*
 * subnetmod -- Vandergaast module. Must be called before validator and 
 * iterator. 
 *
 * Copyright (c) 2013, NLnet Labs.  See LICENSE for license.
 */
 
 /**
 * \file
 * subnet module for unbound.
 */

#include "config.h"

#ifdef CLIENT_SUBNET /* keeps splint happy */

#include <ldns/ldns.h>

#include "edns-subnet/subnetmod.h"
#include "edns-subnet/edns-subnet.h"
#include "edns-subnet/addrtree.h"
#include "services/mesh.h"
#include "services/cache/dns.h"
#include "util/module.h"
#include "util/regional.h"
#include "util/storage/slabhash.h"
#include "util/config_file.h"
#include "util/data/msgreply.h"

/** externally called */
void 
subnet_data_delete(void* d, void* ATTR_UNUSED(arg))
{
	struct subnet_msg_cache_data* r;
	r = (struct subnet_msg_cache_data*)d;
	addrtree_delete(r->tree4);
	addrtree_delete(r->tree6);
	free(r);
}

/** new query for sn */
static int
subnet_new_qstate(struct module_qstate* qstate, int id)
{
	struct subnet_qstate* iq = (struct subnet_qstate*)regional_alloc(
		qstate->region, sizeof(struct subnet_qstate));
	qstate->minfo[id] = iq;
	if(!iq) 
		return 0;
	memset(iq, 0, sizeof(*iq));
	return 1;
}

int subnetmod_init(struct module_env* env, int id)
{
	struct subnet_env* sn_env = (struct subnet_env*)calloc(1,
		sizeof(struct subnet_env));
	if(!sn_env) {
		log_err("malloc failure");
		return 0;
	}
	env->modinfo[id] = (void*)sn_env;
	/* Copy msg_cache settings for now */
	sn_env->subnet_msg_cache = slabhash_create(env->cfg->msg_cache_slabs,
			HASH_DEFAULT_STARTARRAY, env->cfg->msg_cache_size,
			msgreply_sizefunc, query_info_compare,
			query_entry_delete, subnet_data_delete, NULL);
	if(!sn_env->subnet_msg_cache) {
		log_err("subnet: could not create cache");
		return 0;
	}
	return 1;
}

void subnetmod_deinit(struct module_env* env, int id)
{
	struct subnet_env* sn_env;
	if(!env || !env->modinfo[id])
		return;
	sn_env = (struct subnet_env*)env->modinfo[id];
	slabhash_delete(sn_env->subnet_msg_cache);
	free(sn_env);
	env->modinfo[id] = NULL;
}

/** Tells client that upstream has no/improper support */
void cp_edns_bad_response(struct edns_data* target, struct edns_data* source)
{
	target->subnet_scope_mask  = 0;
	target->subnet_source_mask = source->subnet_source_mask;
	target->subnet_addr_fam    = source->subnet_addr_fam;
	memcpy(target->subnet_addr,  source->subnet_addr, INET6_SIZE);
	target->subnet_validdata = 1;
}

void update_cache(struct module_qstate* qstate, int id)
{
	hashvalue_t h;
	struct lruhash_entry* lru_entry;
	struct msgreply_entry* mrep_entry;
	struct addrtree* tree;
	struct reply_info *rep;
	struct query_info qinf;
	struct subnet_msg_cache_data* data;
	struct module_env* env = qstate->env;
	struct subnet_env* sne = (struct subnet_env*)env->modinfo[id];
	struct subnet_qstate* iq = (struct subnet_qstate*)qstate->minfo[id];
	struct slabhash* subnet_msg_cache = sne->subnet_msg_cache;
	struct query_info* qinfo = &qstate->qinfo;
	struct edns_data* edns = &qstate->edns_client_in;
	int acquired_lock = 0;
	
	/** We already calculated hash upon lookup */
	h = iq ? iq->qinfo_hash : query_info_hash(qinfo);
	
	/** Step 1, general qinfo lookup */
	lru_entry = slabhash_lookup(subnet_msg_cache, h, qinfo, 1);
	if (!lru_entry) {
		data = (struct subnet_msg_cache_data*) malloc(
			sizeof(struct subnet_msg_cache_data) );
		if (!data) {
			log_err("malloc failed");
			return;
		}
		data->tree4 = NULL;
		data->tree6 = NULL;
		
		qinf = *qinfo;
		qinf.qname = memdup(qinfo->qname, qinfo->qname_len);
		if(!qinf.qname) {
			log_err("malloc failed");
			free(data);
			return;
		}
		mrep_entry = query_info_entrysetup(&qinf, NULL, h);
		if (!mrep_entry) {
			log_err("query_info_entrysetup failed");
			free(data);
			return;
		}
		free(qinf.qname); /* if qname 'consumed', it is set to NULL */
		lru_entry = &mrep_entry->entry;
		lru_entry->data = data;
		slabhash_insert(subnet_msg_cache, h, lru_entry, data, env->alloc);
	} else {
		data = lru_entry->data;
		acquired_lock = 1;
	}
	/** Step 2, find the correct tree */
	if (edns->subnet_addr_fam == EDNSSUBNET_ADDRFAM_IP4) {
		if (!data->tree4)
			data->tree4 = addrtree_create(EDNSSUBNET_MAX_SUBNET_IP4, env);
		tree = data->tree4;
	} else {
		if (!data->tree6)
			data->tree6 = addrtree_create(EDNSSUBNET_MAX_SUBNET_IP6, env);
		tree = data->tree6;
	}
	assert(tree != NULL); /* TODO remove me */
	if (!tree) {
		log_err("Subnet cache insertion failed");
		if (acquired_lock)
			lock_rw_unlock(&lru_entry->lock);
		return;
	}
	rep = reply_info_copy(qstate->return_msg->rep, env->alloc, NULL);
	/* fixup flags to be sensible for a reply based on the cache */
	rep->flags |= (BIT_RA | BIT_QR);
	rep->flags &= ~(BIT_AA | BIT_CD);

	addrtree_insert(tree, (addrkey_t*)edns->subnet_addr, edns->subnet_source_mask, 
		qstate->edns_server_in.subnet_scope_mask, rep);
	if (acquired_lock)
		lock_rw_unlock(&lru_entry->lock);
}


/* return true iff reply is sent. */
int lookup_and_reply(struct module_qstate* qstate, int id)
{
	struct lruhash_entry* e;
	struct module_env* env = qstate->env;
	struct subnet_env* sne = (struct subnet_env*)env->modinfo[id];
	struct subnet_qstate* iq = (struct subnet_qstate*)qstate->minfo[id];
	hashvalue_t h = query_info_hash(&qstate->qinfo);
	struct subnet_msg_cache_data* data;
	struct edns_data* edns = &qstate->edns_client_in;
	struct addrtree* tree;
	struct addrnode* node;
	struct reply_info* rep;
	
	if (iq) iq->qinfo_hash = h; /** Might be useful on cache miss */
	e = slabhash_lookup(sne->subnet_msg_cache, h, &qstate->qinfo, 0);
	if (!e) return 0; /** qinfo not in cache */
	data = e->data;
	tree = (edns->subnet_addr_fam == EDNSSUBNET_ADDRFAM_IP4)?
		data->tree4 : data->tree6;
	if (!tree) { /** qinfo in cache but not for this family */
		lock_rw_unlock(&e->lock);
		return 0;
	}
	node = addrtree_find(tree, (addrkey_t*)edns->subnet_addr, 
		edns->subnet_source_mask);
	if (!node) { /** plain old cache miss */
		lock_rw_unlock(&e->lock);
		return 0;
	}
	rep = node->elem;
	if(rep->ttl < *env->now) { /** msg expired, remove from node */
		node->elem = NULL;
		addrtree_clean_node(tree, node);
		lock_rw_unlock(&e->lock);
		return 0;
	}
	rep = reply_info_copy(rep, env->alloc, NULL);
	lock_rw_unlock(&e->lock);
	qstate->return_msg = (struct dns_msg*)regional_alloc(
		qstate->region, sizeof(struct dns_msg));
	if (!qstate->return_msg) {
		log_err("subnet: found in cache but alloc failed");
		return 0;
	}
	qstate->return_msg->rep = rep;
	memcpy(&qstate->return_msg->qinfo, &qstate->qinfo, sizeof(struct query_info));
	
	if (edns->subnet_downstream) { /* relay to interested client */
		memcpy(&qstate->edns_client_out, edns, sizeof(struct edns_data));
		qstate->edns_client_out.subnet_scope_mask = node->scope;
	}
	return 1;
}

enum module_ext_state eval_response(struct module_qstate* qstate, int id)
{
	size_t sn_octs;
	struct edns_data *c_in  = &qstate->edns_client_in;
	struct edns_data *c_out = &qstate->edns_client_out;
	struct edns_data *s_in  = &qstate->edns_server_in;
	struct edns_data *s_out = &qstate->edns_server_out;
	
	/** We have not asked for subnet data */
	if (!s_out->subnet_sent) {
		if (s_in->subnet_validdata)
			verbose(VERB_QUERY, "subnet: received spurious data");
		if (c_in->subnet_downstream) /** Copy back to client */
			cp_edns_bad_response(c_out, c_in);
		return module_finished;
	}
	
	/** subnet sent but nothing came back */
	if (!s_in->subnet_validdata) {
		/** The authority indicated no support for vandergaast. As a
		 * consequence the answer ended up in the regular cache. It
		 * is still usefull to put it in the vandergaast cache for 
		 * when a client explicitly asks for subnet specific answer. */
		verbose(VERB_QUERY, "subnet: Authority indicates no support");
		update_cache(qstate, id);
		if (c_in->subnet_downstream)
			cp_edns_bad_response(c_out, c_in);
		return module_finished;
	}
	
	/** Being here means we have asked for and got a subnet specific 
	 * answer. Also, the answer from the authority is not yet cached 
	 * anywhere. */
	
	/** can we accept response? */
	sn_octs = s_out->subnet_source_mask / 8;
	assert(sn_octs <= INET6_SIZE); /** Enforced by msgparse */
	if(s_out->subnet_addr_fam != s_in->subnet_addr_fam ||
		s_out->subnet_source_mask != s_in->subnet_source_mask ||
		memcmp(s_out->subnet_addr, s_in->subnet_addr, sn_octs) != 0 ||
		s_out->subnet_addr[sn_octs] != s_in->subnet_addr[sn_octs])
	{
		/** we can not accept, restart query without option */
		verbose(VERB_QUERY, "subnet: forged data");
		s_out->subnet_validdata = 0;
		s_out->subnet_sent = 0;
		return module_wait_module;
	}
	
	update_cache(qstate, id);
	
	if (c_in->subnet_downstream) {
		/** Client wants to see the answer, echo option back
		 * and adjust the scope. */
		memcpy(c_out, c_in, sizeof(struct edns_data));
		c_out->subnet_scope_mask = s_in->subnet_scope_mask;
	}
	return module_finished;
}

void subnetmod_operate(struct module_qstate* qstate, enum module_ev event, 
	int id, struct outbound_entry* ATTR_UNUSED(outbound))
{
	verbose(VERB_QUERY, "subnet[module %d] operate: extstate:%s "
		"event:%s", id, strextstate(qstate->ext_state[id]), 
		strmodulevent(event));
	log_query_info(VERB_QUERY, "subnet operate: query", &qstate->qinfo);

	if(event == module_event_new) {
		(void) subnet_new_qstate(qstate, id);
		
		if(!qstate->edns_client_in.subnet_validdata) {
			/* No clients are interested in result or we could not
			 * parse it, we don't do client subnet */
			qstate->edns_server_out.subnet_validdata = 0;
			verbose(VERB_ALGO, "subnet: pass to next module");
			qstate->ext_state[id] = module_wait_module;
			return;
		}
		
		if (lookup_and_reply(qstate, id)) {
			verbose(VERB_QUERY, "subnet: answerd from cache");
			qstate->ext_state[id] = module_finished;
			return;
		}
		
		/* copy information from client request to upstream query */
		memcpy(&qstate->edns_server_out, &qstate->edns_client_in, sizeof(struct edns_data));
		qstate->edns_server_out.subnet_scope_mask = 0;
		qstate->edns_server_out.subnet_sent = 0;
		
		/* pass request to next module */
		verbose(VERB_ALGO, "subnet: not found in cache. pass to next module");
		qstate->ext_state[id] = module_wait_module;
		return;
	}
	/* Query handed back by next module, we have a 'final' answer */
	if(event == module_event_moddone) {
		verbose(VERB_QUERY, "subnet: done");
		qstate->ext_state[id] = eval_response(qstate, id);
		return;
	}
	/* We are being revisited */
	if(event == module_event_pass) {
		/* Just pass it on, we already did the work */
		verbose(VERB_ALGO, "subnet: pass to next module");
		qstate->ext_state[id] = module_wait_module;
		return;
	}
	log_err("subnet: bad event %s", strmodulevent(event));
	qstate->ext_state[id] = module_error;
	return;
}

void subnetmod_clear(struct module_qstate* qstate, int id)
{
	/* qstate has no data outside region */
}

void subnetmod_inform_super(struct module_qstate* qstate, int id, 
	struct module_qstate* super)
{
	/* Not used */
}

size_t subnetmod_get_mem(struct module_env* env, int id)
{
	verbose(VERB_ALGO, "subnetmod: get_mem, id: %d, NOTIMPL", id);
	return 550;
}

/**
 * The module function block 
 */
static struct module_func_block subnetmod_block = {
	"subnet", &subnetmod_init, &subnetmod_deinit, &subnetmod_operate,
	&subnetmod_inform_super, &subnetmod_clear, &subnetmod_get_mem
};

struct module_func_block* subnetmod_get_funcblock(void)
{
	return &subnetmod_block;
}

#endif  /* CLIENT_SUBNET */
