#include "Client.h"
#include "server.h"
extern serverInfo server;
extern modLoadOptions servoptions;
Client::Client(int sd, struct sockaddr_in *peer, int instance) {
	connecttime = time(NULL);
	sentconnecttime = NULL;
	game = NULL;
	this->sd = sd;
	memcpy(&sockinfo,peer,sizeof(struct sockaddr_in));
	memcpy(&punching,peer,sizeof(struct sockaddr_in));
	lastPacket = time(NULL);
	version = 0;
	cookie = 0;
	this->instance = instance;
	cindex = 0;
	connected = false;
	gotInit = false;
	gotConnectAck = false;
	deleteMe = false;
}
Client::~Client() {
}
void Client::handleIncoming(char *buff, int len) {
	if(deleteMe) return;
	NatNegPacket *packet = (NatNegPacket *)(buff);
	if(memcmp(packet->magic, NNMagicData, NATNEG_MAGIC_LEN) != 0) { //not a natneg packet
		return; 
	}
	lastPacket = time(NULL);
	switch(packet->packettype) {
		case NN_INIT:
			handleInitPacket(packet);
		break;
		case NN_ADDRESS_CHECK:
			handleAddressCheck(packet);
		break;
		case NN_NATIFY_REQUEST:
			handleNatifyRequest(packet);
		break;
//		case NN_CONNECT_PING: //should never be recieved
		//break;
		case NN_CONNECT_ACK:
			gotConnectAck = true;
			//deleteClient(this);
		break;
		case NN_REPORT:
			handleReport(packet);
		break;
		default:
			printf("unknown natneg packet: %d\n",packet->packettype);
		break;
	}
	
}
void Client::handleReport(NatNegPacket *packet) {
	packet->packettype = NN_REPORT_ACK;
	sendto(sd,(char *)packet,REPORTPACKET_SIZE,0,(struct sockaddr *)&sockinfo,sizeof(struct sockaddr));
}
void Client::handleInitPacket(NatNegPacket *packet) {
	version = packet->version;
	if ((version>1) && (packet->Packet.Init.porttype==1)) {
		Client *c;
		if((c = find_user_by_cookie_index(packet->cookie, instance, packet->Packet.Init.clientindex)) != NULL) {
			if(c != this) {
				punching.sin_port = c->punching.sin_port;
				deleteClient(c);
			}
		}
	}
	cookie = packet->cookie;
	cindex = packet->Packet.Init.clientindex;
	packet->packettype = NN_INITACK;
	sendto(sd,(char *)packet,INITPACKET_SIZE,0,(struct sockaddr *)&sockinfo,sizeof(struct sockaddr));
	if (packet->Packet.Init.porttype>1) {
		//TODO: port guessing
		deleteClient(this);
		return;
	}
	gotInit = true;
	if ((version<2) || (packet->Packet.Init.porttype==1)) trySendConnect();
}
void Client::trySendConnect(bool sendToOther) {
	Client *c;
	if(!gotConnectAck) {
		//TODO: find user by cookie and game
		if((c = find_user_by_cookie_index(cookie, instance,cindex==1?0:1)) != NULL) {
			if(c != this) {
				SendConnectPacket(c,sendToOther);
			}
		}
	}
}
void Client::handleAddressCheck(NatNegPacket *packet) {
	packet->packettype = NN_ADDRESS_REPLY;
	packet->Packet.Init.localip = sockinfo.sin_addr.s_addr;
	packet->Packet.Init.localport = sockinfo.sin_port;
	sendto(sd,(char *)packet,INITPACKET_SIZE,0,(struct sockaddr *)&sockinfo,sizeof(struct sockaddr));
}
void Client::SendConnectPacket(Client *user, bool sendToOther) {
	NatNegPacket sendpacket = {0};
	sendpacket.version = version;
	sendpacket.Packet.Connect.gotyourdata = 'B';
	sendpacket.packettype = NN_CONNECT;
	sendpacket.cookie = cookie;
	memcpy(sendpacket.magic, NNMagicData, NATNEG_MAGIC_LEN);
	sendpacket.Packet.Connect.finished = FINISHED_NOERROR;
	if(instance != 1 || gotConnectAck || user->gotConnectAck || !gotInit) return;
	user->connected = true;
	connected = true;
	sendpacket.Packet.Connect.remoteIP = user->punching.sin_addr.s_addr;
	sendpacket.Packet.Connect.remotePort = user->punching.sin_port;
	sendto(sd,(char *)&sendpacket,CONNECTPACKET_SIZE,0,(struct sockaddr *)&sockinfo,sizeof(struct sockaddr));
	sentconnecttime = time(NULL);
	if(!sendToOther) return;
	user->sentconnecttime = time(NULL);
	sendpacket.Packet.Connect.remoteIP = punching.sin_addr.s_addr;
	sendpacket.Packet.Connect.remotePort = punching.sin_port;
	sendto(user->sd,(char *)&sendpacket,CONNECTPACKET_SIZE,0,(struct sockaddr *)&user->sockinfo,sizeof(struct sockaddr));
}
void Client::handleNatifyRequest(NatNegPacket *packet) {
	SendERTReply(packet->Packet.Init.porttype,packet);
}
void Client::SendERTReply(char type,NatNegPacket *packet) {
	int ertsocket;
	int (*connectFunc)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
	connectFunc = bind; //default to bind(type 2)
	packet->packettype = NN_ERTTEST;
	if(type == 1) {
		sendto(sd,(char *)packet,INITPACKET_SIZE,0,(struct sockaddr *)&sockinfo,sizeof(struct sockaddr));
		return;
	}
	struct sockaddr_in si,si_src;
	if((ertsocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)) == -1) {
		return;
	}
	si.sin_family = AF_INET;
	si.sin_port = sockinfo.sin_port;
	si.sin_addr.s_addr = sockinfo.sin_addr.s_addr;
	si_src.sin_family = AF_INET;
	if(type == NN_PT_NN3) {
		connectFunc = connect;
	}
	si_src.sin_port = htons(MATCHUP_PORT);
	if(type == NN_PT_NN2 || type == NN_PT_NN3) {
		si_src.sin_addr.s_addr = getProbeIP();
	} else {
		si_src.sin_addr.s_addr = getIP(instance);
	}
	if(connectFunc(ertsocket,(struct sockaddr *)&si_src,sizeof(si_src)) != 0) {
		return;
	}
	sendto(ertsocket,(char *)packet,INITPACKET_SIZE,0,(struct sockaddr *)&si,sizeof(struct sockaddr));
	close(ertsocket);
}
void Client::sendDeadBeatNotice() {
	NatNegPacket sendpacket;
	if(instance != 1) return;
	connected = true;
	memset(&sendpacket,0,sizeof(NatNegPacket));
	sendpacket.version = version;
	sendpacket.packettype = NN_CONNECT;
	sendpacket.cookie = cookie;
	memcpy(sendpacket.magic, NNMagicData, NATNEG_MAGIC_LEN);
	sendpacket.Packet.Connect.finished = FINISHED_ERROR_DEADBEAT_PARTNER;
	sendto(sd,(char *)&sendpacket,CONNECTPACKET_SIZE,0,(struct sockaddr *)&sockinfo,sizeof(struct sockaddr));
}
time_t Client::getConnectTime() {
	return connecttime;
}
time_t Client::getLastPacket() {
	return lastPacket;
}
struct sockaddr_in *Client::getSockAddr() {
	return &sockinfo;
}
gameInfo *Client::getGameInfo() {
	return game;
}
uint16_t Client::getPort() {
	return sockinfo.sin_port;
}
uint32_t Client::getAddress() {
	return sockinfo.sin_addr.s_addr;
}
int Client::getSocket() {
	return sd;
}
int Client::getCookie() {
	return cookie;
}
int Client::getInstance() {
	return instance;
}
int Client::getIndex() {
	return cindex;
}
bool Client::getConnected() {
	return connected;
}
bool Client::getConnectedAck() {
	return gotConnectAck;
}
time_t Client::getSendConnectTime() {
	return sentconnecttime;
}
