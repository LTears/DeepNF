#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <unordered_map>
#include "nlohmann/json.hpp"

#include "../../common/RuntimeNode.h"
#include "../../common/MachineConfigurator.h"
#include "../../common/ServiceGraphUtil.h"

using json = nlohmann::json;

MachineConfigurator get_machine_configurator(int port) {
	std::cout << "get machine config called with port: " << port << std::endl;
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		fprintf(stderr, "Cannot open socket\n");
		exit(1);
	}
	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));

	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port);
	servaddr.sin_addr.s_addr = htons(INADDR_ANY);

	if (bind(sockfd, (struct sockaddr*) &servaddr, sizeof(struct sockaddr)) < 0) {
		std::cout << "cannot bind!" << std::endl;
	}

	listen(sockfd, 10);

	const char* ack = "ACK";
	char buffer[1000];

	std::cout << "code here" << std::endl;

	while (true) {
		struct sockaddr_in clientaddr;
		socklen_t clientaddrlen = sizeof(clientaddr);
		int fd = accept(sockfd, (struct sockaddr*) &clientaddr, &clientaddrlen);

		bzero(buffer, sizeof(buffer));
		std::cout << "starting to read from socket" << std::endl;
		int x = read(fd, buffer, sizeof(buffer));
		if (x == 0) {
			std::cout << "read failed" << std::endl;
			break;
		}
		write(fd, ack, strlen(ack));
		sleep(1);
		close(fd);
		break;
	}

	std::string config(buffer, sizeof(buffer));
	//std::cout << "buffer: " << config << std::endl;
	MachineConfigurator *mc = service_graph_util::string_to_machine_configurator(config);
	return *(mc);

	/*RuntimeNode n1 (1, snort);
	RuntimeNode n2 (2, haproxy);
	RuntimeNode n3 (3, snort);
	n1.ip = "173.16.1.2";
	n2.ip = "173.16.1.3";
	n3.ip = "173.16.1.4";
	n1.add_neighbor(2);
	n2.add_neighbor(3);

	Machine m (0);
	m.set_bridge_ip("173.16.1.1");
	m.add_node_id(1);
	m.add_node_id(2);
	m.add_node_id(3);

	MachineConfigurator c (&m);
	
	c.add_node(&n1);
	c.add_node(&n2);
	c.add_node(&n3);

	return c;*/
}

/**
 * Returns a list of nodes on this machine
 */
std::vector<RuntimeNode*> get_internal_nodes(MachineConfigurator c) {
	return c.get_nodes_for_machine(c.get_machine_id());
}

bool is_source_node(RuntimeNode* n, std::vector<RuntimeNode*> nodes) {
	std::set<int> nbrs;
	for (RuntimeNode* n : nodes) {
		std::vector<int> ns = n->get_neighbors();
		nbrs.insert(ns.begin(), ns.end());
	}

	return nbrs.count(n->get_id()) == 0;
}

/**
 * Creates the containers using Dockerfiles.
 */
void setup_nodes(MachineConfigurator conf) {

	std::string to_root = "../../../";
	std::string path_to_merger_dockerfile = "src/runtime/merger/Dockerfile";
	std::string merger_config_dir = "";
	std::vector<RuntimeNode*> nodes = get_internal_nodes(conf);

	// create new containers for classifier and merger
	//system("docker run -d -t -i --name classifier ubuntu /bin/bash");
	//system("docker run -d -t -i --name merger ubuntu /bin/bash");	
	
	/*for (RuntimeNode* n : nodes) {
		int node_id = n->get_id();
		NF node_nf = n->get_nf();

		std::string dockerfile_path = conf.get_dockerfile(node_nf);
		if (dockerfile_path == "") {
			// TODO: raise exception
		}

		std::string config_dir = conf.get_config_dir(node_id);
		conf.make_config_dir(node_id);

		std::string image_name = conf.get_docker_image_name(node_id, node_nf);
		if (image_name == "") {
			// TODO: raise exception
		}
		// store Dockerfile in node config directory
		system(("cp " + dockerfile_path + " " + config_dir).c_str());
		
		// build Docker image with the network function installed and configured
		system(("docker build -t=" + image_name + " " + config_dir).c_str());

		// create a new Docker container for the node
		system(("docker run -d -t -i --name " + n->get_name() + " " + image_name + ":latest /bin/bash").c_str());
	}*/
}

std::unordered_map<int, int> setup_bridge_ports(MachineConfigurator &conf) {

	// create a bridge
	system("sudo \"PATH=$PATH\" /home/ec2-user/ovs/utilities/ovs-vsctl add-br ovs-br");
	
	// get bridge ip
	Machine* cur_machine = conf.get_machine_with_id(conf.get_machine_id());
	std::string bridge_ip = cur_machine->get_bridge_ip();
	
	system(("sudo ifconfig ovs-br " + bridge_ip + " netmask 255.255.255.0 up").c_str());

	// getting ip info from bridges
	int dotinx = bridge_ip.rfind(".");
	std::string ip_assign = bridge_ip.substr(0, dotinx+1);
	int ofport_inx = atoi(bridge_ip.substr(dotinx+1).c_str());
	int ip_inx = ofport_inx + 1;

	// connect containers to the bridge
	// std::string add_port_classifier = "sudo \"PATH=$PATH\" /home/ec2-user/ovs/utilities/ovs-docker add-port ovs-br eth1 classifier";
	//std::string add_port_merger = "sudo \"PATH=$PATH\" /home/ec2-user/ovs/utilities/ovs-docker add-port ovs-br eth1 merger";
	//system(add_port_classifier.c_str());
	//system(add_port_merger.c_str());

	std::vector<RuntimeNode*> nodes = get_internal_nodes(conf);

	/* MERGER PORT SETUP */
	// node id to eth setup map
	std::unordered_map<int, int> nodeid_to_eth;
	std::string add_port_merger = "sudo \"PATH=$PATH\" /home/ec2-user/ovs/utilities/ovs-docker add-port ovs-br eth";
	int eth_inx = 1;
	auto j = json::object();
	for (RuntimeNode* n : nodes) {
		if (n->get_neighbors().size() == 0) {
			std::string command = add_port_merger + std::to_string(eth_inx) + " --ipaddress=" + 
				ip_assign + std::to_string(ip_inx) + " " + "merger";
			std::cout << "command: " << command << std::endl;
			nodeid_to_eth[n->get_id()] = eth_inx;
			j["eth" + std::to_string(eth_inx)] = n->get_name();
			system(command.c_str());
			eth_inx++;
		}
	}

	std::ofstream out("../../../src/common/eth_leaf_map.json");
    out << j;

	//int ofport = 3;
	std::string add_port_command = "sudo \"PATH=$PATH\" /home/ec2-user/ovs/utilities/ovs-docker add-port ovs-br";
	for (RuntimeNode* n : nodes) {
		//NOTE: Docker containers must be named the same as functions

		std::string command1 = add_port_command + " eth1 " + n->get_name();
		std::string command2 = add_port_command + " eth2 " + n->get_name();
		std::cout << "command: " << command1 << std::endl;
		std::cout << "command: " << command2 << std::endl;
		system(command1.c_str());
		n->inport = eth_inx ++;
		system(command2.c_str());
		n-> outport = eth_inx ++;
		// std::string node_id = std::to_string(n.get_id());
		/*switch(n->get_nf()) {
			case snort:
				system((add_port_command + " eth1 " + n->get_name()).c_str());
				n->inport = ofport ++;
				system((add_port_command + " eth2 " + n->get_name()).c_str());
				n->outport = ofport ++;
				break;
			
			case haproxy:
				system((add_port_command + " eth1 " + n->get_name() + " --ipaddress=" + n->ip).c_str());
				n->inport = ofport ++;
				n->outport = n->inport;
				break;
			
			default: break;
		}*/
	}

	return nodeid_to_eth;
}

/**
 * Adds OVS flow rules between the containers.
 * reference: https://paper.dropbox.com/doc/Flows-in-OpenVSwitch-nVRg9phHBr5JSZO2vFwCJ?_tk=share_copylink
 */
void make_flow_rules(MachineConfigurator conf, std::unordered_map<int,int> leaf_to_eth) {
	std::string add_flow_command = "sudo \"PATH=$PATH\" /home/ec2-user/ovs/utilities/ovs-ofctl add-flow ovs-br in_port=";
	
	std::vector<RuntimeNode*> nodes = get_internal_nodes(conf);
	std::vector<int> source_node_inports;
	
	for (RuntimeNode* n : nodes) {
		
		if (is_source_node(n, nodes)) { // flow from classifier to this node
			source_node_inports.push_back(n->inport);
			// system((add_flow_command + "1,actions=" + std::to_string(n.inport)).c_str());
		}

		if (n->get_neighbors().size() == 0) { // if node is a sink, flow from this node to merger
			int merger_port = leaf_to_eth[n->get_id()];
			system((add_flow_command + std::to_string(n->outport) + ",actions=" + std::to_string(merger_port)).c_str());
		} else { // flow from output port of this node to all its successors ports
			std::vector<int> neighbors = n->get_neighbors();
			std::string outport_ports = "";
			for (int j = 0; j < (int) neighbors.size(); j++) {
				RuntimeNode* neighbor = conf.get_node_with_id(neighbors[j]);
				outport_ports += std::to_string(neighbor->inport);
				if (j < (int)neighbors.size() - 1) {
					outport_ports += ",";
				}
			}
			system((add_flow_command + std::to_string(n->outport) + ",actions=" + outport_ports).c_str());
		}
	}
	std::string outport_ports = "";
	int i = 0;
	for (int p : source_node_inports) {
		if (i == (int) source_node_inports.size() - 1) {
			outport_ports += std::to_string(p);
		} else {
			outport_ports += std::to_string(p) + ",";
		}
		i++;
	}

	//system((add_flow_command + "1,actions=" + outport_ports).c_str());
}

/**
 * Runs the NF on each node
 */
void start_network_functions(MachineConfigurator c) {
	std::string docker_exec_command = "docker exec -d -t -i ";
	std::vector<RuntimeNode*> nodes = get_internal_nodes(c);
	std::string exec_nf_cmd;
	for (RuntimeNode* n : nodes) {
		std::string exec_nf_cmd = docker_exec_command + n->get_name() + " ";
		switch(n->get_nf()) {
		case snort:
			exec_nf_cmd += "snort -N -A console -q -c /etc/snort/snort.conf -Q -i eth1:eth2"; 
			break;
		case haproxy:
			exec_nf_cmd += "service haproxy start";
			break;
		}
		system(exec_nf_cmd.c_str());
	}
}

void reset(MachineConfigurator c) {
	std::string del_ports_cmd = "sudo \"PATH=$PATH\" /home/ec2-user/ovs/utilities/ovs-docker del-ports ovs-br ";

	// clean up merger and classifier
	system((del_ports_cmd + "classifier").c_str());
	system((del_ports_cmd + "merger").c_str());
	system("docker stop classifier merger; docker rm classifier merger");

	std::vector<RuntimeNode*> nodes = get_internal_nodes(c);
	for (RuntimeNode* n : nodes) {
		// remove all veth pairs for this node
		system((del_ports_cmd + n->get_name()).c_str());
		// stop and remove the docker container for this node
		system(("docker stop " + n->get_name() + "; docker rm " + n->get_name()).c_str());
	}

	// delete the bridge
	system("sudo \"PATH=$PATH\" /home/ec2-user/ovs/utilities/ovs-vsctl del-br ovs-br");
}

/**
 * Takes in node info, NF config files and flow rules from user and automates the setup of runtime components.
 * Optional flag -r to remove all resources and undo the configuration.
 */
int main(int argc, char *argv[]) {
	int c;
	int port = 10000;
	opterr = 0;
	bool needReset = false;

	while ((c = getopt(argc, argv, "p:r")) != -1) {
		switch(c) {
			case 'p':
				port = atoi(optarg);
				break;
			case 'r':
				needReset = true;
				break;
		}
	}

	MachineConfigurator conf = get_machine_configurator(port);
	/*int machineId = conf.get_machine_id();
	Machine* mac = conf.get_machine_with_id(machineId);
	std::cout << mac->get_bridge_ip() << std::endl;*/

	if (needReset) {
		reset(conf);
	} else {
		// making a dummy service graph
        std::cout << "graph here!" << std::endl;
		/*setup_nodes(conf);
		std::unordered_map<int, int> leaf_to_eth = setup_bridge_ports(conf);
		make_flow_rules(conf, leaf_to_eth);
		start_network_functions(conf);*/
	}

	return 0;
}

