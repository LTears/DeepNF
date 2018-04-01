//
// Created by Victoria on 2018-03-31.
//

#include "MergerOperator.h"


std::string packet_filter_expr = "tcp";


void process_packet_handler(unsigned char * user,
                            const struct pcap_pkthdr *pkthdr,
                            const unsigned char * packet) {
    ((MergerOperator*) user)->process_packet(user, pkthdr, packet);
}


MergerOperator::MergerOperator(std::string action_file_path) {
    // set up action table
    nlohmann::json action_table;
    std::ifstream action_table_input(action_file_path);
    action_table_input >> action_table;
    this->action_table_helper = new ActionTableHelper(action_table);
}


/* FUNCTIONS FOR PERFORMING MERGER OPERATIONS */

/* sets up hardcoded MergerInfo object to do testing on */
MergerInfo* MergerOperator::setup_dummy_info() {
    RuntimeNode n1 (1, snort); // node that drops packets
    RuntimeNode n2 (2, snort); // node that sends packets

    std::map<std::string, RuntimeNode*> interface_leaf_map;
    interface_leaf_map.insert(std::make_pair("eth1", &n1));
    interface_leaf_map.insert(std::make_pair("eth2", &n1));

    std::vector<ConflictItem*> conflicts_list;
    std::vector<RuntimeNode*> service_graph;

    MergerInfo* mi = new MergerInfo(interface_leaf_map, conflicts_list, service_graph);
    return mi;
}


void MergerOperator::run() {
    printf("wtf is this bullshit\n");

    // create dummy MergerInfo object
    this->merger_info = setup_dummy_info();

    this->logfile = fopen("log.txt","w");
    if (this->logfile == NULL) printf("Unable to create file.");

    // create pcap handles
    std::string dst_dev = "eth3"; // destination to send packets after merge

    configure_device_read_handles(packet_filter_expr);
    configure_device_write_handle(packet_filter_expr, dst_dev);

    printf("finished setting up bullshit\n");

    // listen for and process incoming packets
    for (std::map<std::string, RuntimeNode*>::iterator it = this->merger_info->get_interface_leaf_map().begin();
         it != this->merger_info->get_interface_leaf_map().end(); ++it) {
        /* loop for callback function */
        cur_dev = it->first;
        pcap_loop(src_dev_handle_map[it->first], 3, process_packet_handler, (u_char*) this);
    }
}

void MergerOperator::process_packet(u_char *arg,
                    const struct pcap_pkthdr* pkthdr,
                    const u_char* packet)
{
    printf("Received new packet\n");

    struct packet *pkt_info = new struct packet(packet, pkthdr->len);

    this->print_ip_header(pkt_info->ip_header);
    this->print_tcp_packet(pkt_info->tcp_header);
    this->print_data(pkt_info->data, pkt_info->data_size);

    int packet_id = ntohs(pkt_info->ip_header->ip_id);

    /* add packet to the map */
    std::map<int, NFPacket*>* runtime_pkt_map;
    if (packet_map.count(packet_id) > 0) {
        runtime_pkt_map = packet_map[packet_id];
    } else {
        runtime_pkt_map = new std::map<int, NFPacket*>();
    }

    NFPacket p;
    p.pkt = pkt_info;
    RuntimeNode* n = this->merger_info->get_interface_leaf_map().at(cur_dev);
    p.runtime_id = n->get_id();
    p.nf = n->get_nf();

    runtime_pkt_map->insert(std::make_pair(p.runtime_id, &p));
    packet_map[packet_id] = runtime_pkt_map;

    // if all packets have been received for the given id, begin merging
    if (packet_map[packet_id]->size() == merger_info->get_interface_leaf_map().size()) {
        NFPacket* merged_packet = merge_all(packet_id);

        // send packet to destination virtual interface
        if (!merged_packet->pkt->is_null()) {
            if (pcap_sendpacket(this->dst_dev_handle, merged_packet->pkt->pkt, merged_packet->pkt->size) < 0) {
                std::cerr << strerror(errno) << std::endl;
            }
        }

        // cleanup
        packet_map.erase(packet_id);
    }

}


MergerOperator::NFPacket* MergerOperator::resolve_packet_conflict(
        NFPacket* major_p,
        NFPacket* minor_p,
        ConflictItem* conflict)
{
    // if either packet is null (was dropped), return null packet
    if (major_p->pkt->is_null() || minor_p->pkt->is_null()) {
        return copy_nfpacket(major_p);
    }

    // otherwise, merge packet based on conflicting items
    struct packet *new_pkt = new struct packet(major_p->pkt, major_p->pkt->size);
    std::map<NF, std::set<Field>> fields_map = action_table_helper->get_write_fields_map();

    // write changes made in minor packet
    std::set<Field> major_fields = fields_map[major_p->nf];
    for (std::set<Field>::iterator it = major_p->written_fields.begin();
         it != major_p->written_fields.end(); ++it) {
        major_fields.insert(*it);
    }

    std::set<Field> minor_fields = fields_map[minor_p->nf];
    for (std::set<Field>::iterator it = minor_p->written_fields.begin();
         it != minor_p->written_fields.end(); ++it) {
        minor_fields.insert(*it);
    }


    for (std::set<Field>::iterator it = minor_fields.begin(); it != minor_fields.end(); ++it) {
        Field field = *it;

        // write the minor's field changes as long as the change does NOT conflict with major
        if (major_fields.find(field) == major_fields.end()) {
            switch (field) {
                case Field::SIP:
                    new_pkt->ip_header->ip_src = minor_p->pkt->ip_header->ip_src;
                    break;

                case Field::DIP:
                    new_pkt->ip_header->ip_dst = minor_p->pkt->ip_header->ip_dst;
                    break;

                case Field::SPORT:
                    new_pkt->tcp_header->th_sport = minor_p->pkt->tcp_header->th_sport;
                    break;

                case Field::DPORT:
                    new_pkt->tcp_header->th_dport = minor_p->pkt->tcp_header->th_dport;
                    break;

                case Field::PAYLOAD:
                    new_pkt->data = minor_p->pkt->data;
                    new_pkt->data_size = minor_p->pkt->data_size;
                    break;
            }
        }
    }

    NFPacket ret_p;
    ret_p.pkt = new_pkt;
    ret_p.runtime_id = conflict->get_parent();
    ret_p.nf = merger_info->get_node_map()[ret_p.runtime_id]->get_nf();

    // construct written_fields set for merged packet
    std::set<Field> written_fields;
    for (std::set<Field>::iterator it = minor_fields.begin();
         it != minor_fields.end(); ++it) {
        ret_p.written_fields.insert(*it);
    }
    for (std::set<Field>::iterator it = major_fields.begin();
         it != major_fields.end(); ++it) {
        ret_p.written_fields.insert(*it);
    }

    return &ret_p;
}


// returns merged packet for the given packet ID. Assumes that all packets for the packet ID have been received
MergerOperator::NFPacket* MergerOperator::merge_all(int pkt_id) {
    // maps each runtime leaf id with its associated packet
    std::map<int, NFPacket*>* rt_to_pkt_map = packet_map[pkt_id];

    // list of conflict runtime ids
    std::vector<ConflictItem*> conflicts_list = merger_info->get_conflicts_list();

    bool was_changed = true; // has at least one merge conflict been resolved in this iteration?
    bool major_exists;
    bool minor_exists;

    // iterate through conflicts list and resolve conflicts
    while (was_changed) {
        was_changed = false;
        for (std::vector<ConflictItem*>::iterator it = conflicts_list.begin(); it != conflicts_list.end(); ++it) {
            ConflictItem* ci = *it;
            major_exists = rt_to_pkt_map->find(ci->get_major()) != rt_to_pkt_map->end();
            minor_exists = rt_to_pkt_map->find(ci->get_minor()) != rt_to_pkt_map->end();

            // if both major and minor of the conflict item exist in the map, merge them
            if (major_exists && minor_exists) {
                was_changed = true;

                NFPacket* merged_pkt = this->resolve_packet_conflict((*rt_to_pkt_map)[ci->get_major()],
                                                                     (*rt_to_pkt_map)[ci->get_minor()],
                                                                     ci);

                // if merged_pkt is null, drop and end merging process
                if (merged_pkt->pkt->is_null()) {
                    return merged_pkt;
                }

                // remove major and minor from packet_map and add merged pkt
                rt_to_pkt_map->erase(ci->get_major());
                rt_to_pkt_map->erase(ci->get_minor());
                rt_to_pkt_map->insert(std::make_pair(merged_pkt->runtime_id, merged_pkt));
            }
        }
    }

    // merging process should be finished by now
    if (rt_to_pkt_map->size() != 1) {
        throw std::runtime_error("More than one packet exists in pkt map even thought merging is finished");
    }

    for (std::map<int, NFPacket*>::iterator it = rt_to_pkt_map->begin(); it != rt_to_pkt_map->end(); ++it) {
        NFPacket* final_pkt = it->second;
        return final_pkt;
    }
}


/* HELPER FUNCTIONS FOR OPERATING ON PACKETS */
void MergerOperator::configure_device_read_handles(std::string packet_filter_expr)
{

    std::map<std::string, RuntimeNode*> interface_leaf_map = this->merger_info->get_interface_leaf_map();

    for (std::map<std::string, RuntimeNode*>::iterator it = interface_leaf_map.begin();
        it != interface_leaf_map.end(); ++it) {

        std::string dev = it->first;

        char errbuf[PCAP_ERRBUF_SIZE];
        struct bpf_program fp;        /* hold compiled program */
        bpf_u_int32 maskp;            /* subnet mask */
        bpf_u_int32 netp;             /* ip */

        if (pcap_lookupnet(dev.c_str(), &netp, &maskp, errbuf) < 0) {
            std::cerr << errbuf << std::endl;
            exit(-1);
        }

        /* open device for reading in promiscuous mode */
        src_dev_handle_map[dev] = pcap_open_live(dev.c_str(), BUFSIZ, 1,-1, errbuf);

        if(src_dev_handle_map[dev] == NULL) {
            printf("pcap_open_live(): %s\n", errbuf);
            exit(-1);
        }

        /* Now we'll compile the filter expression*/
        if(pcap_compile(src_dev_handle_map[dev], &fp, packet_filter_expr.c_str(), 0, netp) == -1) {
            fprintf(stderr, "Error calling pcap_compile\n");
            exit(-1);
        }

        /* set the filter */
        if(pcap_setfilter(src_dev_handle_map[dev], &fp) == -1) {
            fprintf(stderr, "Error setting filter\n");
            exit(-1);
        }
    }
}


void MergerOperator::configure_device_write_handle(std::string packet_filter_expr,
                                   std::string dev)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    const u_char *packet;
    struct bpf_program fp;        /* hold compiled program */
    bpf_u_int32 maskp;            /* subnet mask */
    bpf_u_int32 netp;             /* ip */

    if (pcap_lookupnet(dev.c_str(), &netp, &maskp, errbuf) < 0) {
        std::cerr << errbuf << std::endl;
    }


    /* open device for reading in promiscuous mode */
    dst_dev_handle = pcap_open_live(dev.c_str(), BUFSIZ, 1,-1, errbuf);

    if(dst_dev_handle == NULL) {
        printf("pcap_open_live(): %s\n", errbuf);
        exit(-1);
    }
}


MergerOperator::NFPacket* MergerOperator::copy_nfpacket(NFPacket* nfpacket) {
    NFPacket copy;
    copy.pkt = nfpacket->pkt->copy();
    copy.runtime_id = nfpacket->runtime_id;
    return &copy;
}


/* HELPER FUNCTIONS THAT PRINT STUFF */
void MergerOperator::print_tcp_packet(struct tcphdr *tcph)
{
    fprintf(this->logfile,"\n");
    fprintf(this->logfile,"TCP Header\n");
    fprintf(this->logfile,"   |-Source Port      : %u\n",ntohs(tcph->th_sport));
    fprintf(this->logfile,"   |-Destination Port : %u\n",htons(tcph->th_dport));
    fprintf(this->logfile,"   |-Sequence Number    : %u\n",ntohl(tcph->th_seq));
    fprintf(this->logfile,"   |-Acknowledge Number : %u\n",ntohl(tcph->th_ack));
    fprintf(this->logfile,"\n");
    fprintf(this->logfile,"                        DATA Dump                         ");
    fprintf(this->logfile,"\n");

}



void MergerOperator::print_ip_header(struct ip *iph)
{
    char sourceIp[INET_ADDRSTRLEN];
    char destIp[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &(iph->ip_src), sourceIp, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &(iph->ip_dst), destIp, INET_ADDRSTRLEN);

    fprintf(this->logfile,"\n");
    fprintf(this->logfile,"IP Header\n");
    fprintf(this->logfile,"   |-IP Version        : %d\n",(unsigned int)iph->ip_v);
    fprintf(this->logfile,"   |-IP Header Length  : %d DWORDS or %d Bytes\n",(unsigned int)iph->ip_hl,((unsigned int)(iph->ip_hl))*4);
    fprintf(this->logfile,"   |-Type Of Service   : %d\n",(unsigned int)iph->ip_tos);
    fprintf(this->logfile,"   |-IP Total Length   : %d  Bytes(Size of Packet)\n",ntohs(iph->ip_len));
    fprintf(this->logfile,"   |-Identification    : %d\n",ntohs(iph->ip_id));
    //fprintf(this->logfile,"   |-Reserved ZERO Field   : %d\n",(unsigned int)iphdr->ip_reserved_zero);
    //fprintf(this->logfile,"   |-Dont Fragment Field   : %d\n",(unsigned int)iphdr->ip_dont_fragment);
    //fprintf(this->logfile,"   |-More Fragment Field   : %d\n",(unsigned int)iphdr->ip_more_fragment);
    fprintf(this->logfile,"   |-TTL      : %d\n",(unsigned int)iph->ip_ttl);
    fprintf(this->logfile,"   |-Protocol : %d\n",(unsigned int)iph->ip_p);
    fprintf(this->logfile,"   |-Checksum : %d\n",ntohs(iph->ip_sum));
    fprintf(this->logfile,"   |-Source IP        : %s\n", sourceIp);
    fprintf(this->logfile,"   |-Destination IP   : %s\n", destIp);
}

void MergerOperator::print_data(u_char* data, int size)
{
    std::string dataStr = "";

    /* convert non-printable characters, other than carriage return, line feed,
     * or tab into periods when displayed. */
    for (int i = 0; i < size; i++) {
        if ((data[i] >= 32 && data[i] <= 126) || data[i] == 10 || data[i] == 11 || data[i] == 13) {
            dataStr += (char)data[i];
        } else {
            dataStr += ".";
        }
    }

    fprintf(this->logfile,"Data Payload\n%s", dataStr.c_str());

    fprintf(this->logfile,"\n###########################################################\n");
}
