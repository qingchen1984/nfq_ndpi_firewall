#include <stdio.h>
#include <stdlib.h>

#include "ndpi_main.h"
#include "ndpi_helper.h"

/*
 *  malloc wrapper function
 */
void *malloc_wrapper(size_t size) 
{
    return malloc(size);
}

/*
 *  free wrapper function
 */
void free_wrapper(void *freeable) 
{
    free(freeable);
}

/*
 *  setup detection
 */
struct ndpi_detection_module_struct *setup_detection(void *data)
{
NDPI_PROTOCOL_BITMASK all;
    
    set_ndpi_malloc(malloc_wrapper), set_ndpi_free(free_wrapper);
    set_ndpi_flow_malloc(NULL), set_ndpi_flow_free(NULL);

    struct ndpi_detection_module_struct *ndpi_struct = ndpi_init_detection_module();

    if (ndpi_struct == NULL) {
     	NDPI_LOG(0, NULL, NDPI_LOG_ERROR, "global structure initialization failed\n");
	exit(1);
    }

    NDPI_BITMASK_SET_ALL(all);
    ndpi_set_protocol_detection_bitmask2(ndpi_struct, &all);

    return ndpi_struct;
}

int ndpi_workflow_node_cmp(const void *a, const void *b) {
    struct flow_info *flow_a = (struct flow_info*)a;
    struct flow_info *flow_b = (struct flow_info*)b;

    if (flow_a->hash_value < flow_b->hash_value) {
	return(-1); 
    } else if (flow_a->hash_value > flow_b->hash_value) {
	return(1);
    }

    /* Flows have the same hash */

    if (flow_a->protocol < flow_b->protocol) {
	return(-1); 
    } else if (flow_a->protocol > flow_b->protocol) {
	return(1);
    }

    if(((flow_a->src_ip == flow_b->src_ip)&& 
		(flow_a->src_port == flow_b->src_port) && 
		(flow_a->dst_ip == flow_b->dst_ip) && 
		(flow_a->dst_port == flow_b->dst_port)) ||
	    ((flow_a->src_ip == flow_b->dst_ip) && 
	     (flow_a->src_port == flow_b->dst_port) && 
	     (flow_a->dst_ip == flow_b->src_ip) && 
	     (flow_a->dst_port == flow_b->src_port))) {
	return(0);    
    }


    if (flow_a->src_ip < flow_b->src_ip) {
	return(-1); 
    } else if (flow_a->src_ip > flow_b->src_ip) {
	return(1);
    }

    if (flow_a->src_port < flow_b->src_port) {
	return(-1); 
    } else if (flow_a->src_port > flow_b->src_port) {
	return(1);
    }

    if (flow_a->dst_ip < flow_b->dst_ip) {
	return(-1); 
    } else if (flow_a->dst_ip > flow_b->dst_ip) {
	return(1);
    }

    if (flow_a->dst_port < flow_b->dst_port) {
	return(-1); 
    } else if (flow_a->dst_port > flow_b->dst_port) {
	return(1);
    }

    return(0); /* should not be reached */
}

static struct flow_info *
get_flow_info(struct ndpi_workflow *workflow, const struct ndpi_iphdr *iph,
		    u_int16_t ipsize, struct ndpi_id_struct **src,
		    struct ndpi_id_struct **dst, u_int8_t *proto)
{
    u_int32_t idx, l4_offset, hashval;
    int l4_packet_len;
    void *search_res;
    u_int8_t *l3, *l4;

    struct ndpi_tcphdr *tcph = NULL;
    struct ndpi_udphdr *udph = NULL;
    u_int16_t sport, dport;

    l4_offset = iph->ihl * 4;
    l3 = (u_int8_t*)iph;

    *proto = iph->protocol;
    l4 = ((u_int8_t *)l3 + l4_offset);

    l4_packet_len = ntohs(iph->tot_len) - (iph->ihl * 4);

    if (iph->protocol == IPPROTO_TCP && l4_packet_len >= 20) {
	tcph = (struct ndpi_tcphdr *)l4;
	sport = ntohs(tcph->source); 
	dport = ntohs(tcph->dest);
    } else if (iph->protocol == IPPROTO_UDP && l4_packet_len >= 8) {
	udph = (struct ndpi_udphdr *)l4;
	sport = ntohs(udph->source); 
	dport = ntohs(udph->dest);
    } else {
	// non tcp/udp protocols
	sport = dport = 0;
    }

    struct flow_info *flow = malloc(sizeof(struct flow_info));
    if (flow == NULL) {
	printf("ERROR: flow could be allocated.\n");
	exit(1);
    } else {
	memset(flow, 0, sizeof(struct flow_info));
    }

    flow->protocol = iph->protocol;
    flow->src_ip = iph->saddr; 
    flow->dst_ip = iph->daddr;
    flow->src_port = htons(sport); 
    flow->dst_port = htons(dport);
    
    hashval = flow->protocol + flow->src_ip + flow->dst_ip + flow->src_port + flow->dst_port;
    flow->hash_value = hashval;
    
    idx = hashval % workflow->num_roots;
    
    // search for a flow in the tree
    search_res = ndpi_tfind(flow, &workflow->ndpi_flows_root[idx], ndpi_workflow_node_cmp);

    struct flow_info *ret;
    
    if (search_res != NULL) {
	// flow was found
	
	ret = *(struct flow_info**)search_res;
	if(ret->src_ip == iph->saddr && 
		ret->src_ip == iph->daddr && 
		ret->dst_port == htons(sport) && 
		ret->dst_port == htons(dport)) {
	    *src = ret->src_id;
	    *dst = ret->dst_id;
	} else {
	    *src = ret->dst_id;
	    *dst = ret->src_id;
	}
    } else {
	// create new flow
	
	if (workflow->flow_count > workflow->max_flows) {
	    printf("ERROR: max number of flows was exceeded.\n");
	    ret = NULL;
	} else {
	    ret = flow;

	    ret->ndpi_flow = ndpi_flow_malloc(SIZEOF_FLOW_STRUCT);
	    if (ret->ndpi_flow == NULL) {
		printf("ERROR: not enough memory to create a new ndpi flow.\n");
		return NULL;
	    }
	    memset(ret->ndpi_flow, 0, SIZEOF_FLOW_STRUCT);

	    ret->src_id = ndpi_flow_malloc(SIZEOF_ID_STRUCT);
	    if (ret->src_id == NULL) {
		printf("ERROR: not enough memory to create a new src_id.\n");
		return NULL;
	    }
	    memset(ret->src_id, 0, SIZEOF_ID_STRUCT);

	    ret->dst_id = ndpi_flow_malloc(SIZEOF_ID_STRUCT);
	    if (ret->dst_id == NULL) {
		printf("ERROR: not enough memory to create a new dst_id.\n");
		return NULL;
	    }
	    memset(ret->dst_id, 0, SIZEOF_ID_STRUCT);

	    // add new flow to the tree
	    ndpi_tsearch(ret, &workflow->ndpi_flows_root[idx], ndpi_workflow_node_cmp);
	    workflow->flow_count++;

	    *src = ret->src_id;
	    *dst = ret->dst_id;
	}
    }
    
    return ret;
}

void free_flow_partially(struct flow_info *flow) {
    if(flow->ndpi_flow != NULL) { 
	ndpi_flow_free(flow->ndpi_flow); 
	flow->ndpi_flow = NULL; 
    }

    if(flow->src_id) { 
	ndpi_free(flow->src_id); 
	flow->src_id = NULL; 
    }

    if(flow->dst_id) { 
	ndpi_free(flow->dst_id); 
	flow->dst_id = NULL; 
    }
}

void process_ndpi_collected_info(struct ndpi_workflow * workflow, struct flow_info *flow) {
    if(!flow->ndpi_flow) {
	printf("ERROR: flow is NULL \n");
	exit(1);
    }

    if(flow->detected_protocol.app_protocol != NDPI_PROTOCOL_DNS) {
	/* SSH */
	if (flow->detected_protocol.app_protocol == NDPI_PROTOCOL_SSH) {
	    snprintf(flow->ssh_ssl.client_info, sizeof(flow->ssh_ssl.client_info), "%s",
		    flow->ndpi_flow->protos.ssh.client_signature);

	    snprintf(flow->ssh_ssl.server_info, sizeof(flow->ssh_ssl.server_info), "%s",
		    flow->ndpi_flow->protos.ssh.server_signature);
	} else if ((flow->detected_protocol.app_protocol == NDPI_PROTOCOL_SSL) || 
		(flow->detected_protocol.master_protocol == NDPI_PROTOCOL_SSL)) {
	    snprintf(flow->ssh_ssl.client_info, sizeof(flow->ssh_ssl.client_info), "%s",
		    flow->ndpi_flow->protos.ssl.client_certificate);

	    snprintf(flow->ssh_ssl.server_info, sizeof(flow->ssh_ssl.server_info), "%s",
		    flow->ndpi_flow->protos.ssl.server_certificate);
	}
    }

    if(flow->detection_completed) {
	free_flow_partially(flow);
    }
}

static struct ndpi_proto 
process_pkt(struct ndpi_workflow *workflow, 
	    const unsigned char *packet, 
	    const unsigned short packetlen, 
	    struct timeval timestamp)
{
    struct ndpi_iphdr *iph; 
    struct ndpi_id_struct *src, *dst;
    struct ndpi_flow_struct *ndpi_flow = NULL;
    int ip_offset = 0;

    u_int8_t ip_proto;

    iph = (struct ndpi_iphdr *) &packet[ip_offset];

    struct flow_info *flow = malloc(sizeof(struct flow_info));
    if (flow == NULL) {
	printf("ERROR: flow could be allocated.\n");
	exit(1);
    } else {
	memset(flow, 0, sizeof(struct flow_info));
    }

    flow = get_flow_info(workflow, iph, packetlen, &src, &dst, &ip_proto);

    if(flow != NULL) {
	ndpi_flow = flow->ndpi_flow;
	flow->packets++;
	flow->last_seen = timestamp;
    } else {
	printf("ERROR: an error occured during get_flow_info.\n");
	exit(1);
    }

    u_int64_t tick = ((uint64_t) timestamp.tv_sec) * TICK_RESOLUTION + 
	timestamp.tv_usec / (1000000 / TICK_RESOLUTION);

    if(flow->detection_completed == 0) {
	flow->detected_protocol = ndpi_detection_process_packet(workflow->ndpi_struct, 
								ndpi_flow,
								(uint8_t *)iph, 
								packetlen,
								tick, 
								src, dst);
	if ((flow->detected_protocol.app_protocol != NDPI_PROTOCOL_UNKNOWN) || 
		((ip_proto == IPPROTO_UDP) && (flow->packets > 8)) || 
		((ip_proto == IPPROTO_TCP) && (flow->packets > 10))) {
	    /* New protocol detected or give up */
	    flow->detection_completed = 1;

	    if (flow->detected_protocol.app_protocol == NDPI_PROTOCOL_UNKNOWN) {
		flow->detected_protocol = ndpi_detection_giveup(workflow->ndpi_struct, flow->ndpi_flow);
	    }

	    process_ndpi_collected_info(workflow, flow);
	}  
    }

    return flow->detected_protocol;
}

struct ndpi_proto detect_protocol(const unsigned char *packet, 
			    const unsigned short packetlen, 
			    struct timeval timestamp,
			    struct ndpi_workflow *workflow)
{
    struct ndpi_proto detected_protocol;

    detected_protocol = process_pkt(workflow, packet, packetlen, timestamp);

    return detected_protocol;
}

void print_proto_names(struct ndpi_detection_module_struct *ndpi_struct)
{
    int i = 0;
    for (i = 0; i <= 227; i++) {
	printf("%s\n", ndpi_get_proto_name(ndpi_struct, i));
    }
}
