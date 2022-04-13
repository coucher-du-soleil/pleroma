#include "netcode.h"
#include "../shared_src/protoloma.pb.h"
#include "concurrentqueue.h"
#include "hylic_ast.h"
#include "pleroma.h"
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <enet/enet.h>
#include <enet/types.h>
#include <map>
#include <string>
#include <vector>

moodycamel::ConcurrentQueue<Msg> net_out_queue;
std::queue<Msg> net_in_queue;

moodycamel::ConcurrentQueue<Vat*> net_vats;

struct PleromaNetwork {
  ENetHost *server;
  std::map<enet_uint32, ENetPeer *> peers;
  std::map<int, enet_uint32> node_host_map;
} pnet;

void net_loop() {
  ENetEvent event;
  romabuf::PleromaMessage message;

  /* Wait up to 1000 milliseconds for an event. */
  while (enet_host_service(pnet.server, &event, 1000) > 0) {
    switch (event.type) {
    case ENET_EVENT_TYPE_CONNECT:
      printf("Got connection.\n");
      if (pnet.peers.find(event.peer->address.host) == pnet.peers.end()) {
        printf("Connecting to...\n");
        connect_client(event.peer->address);
      }

      send_msg(event.peer->address.host, message);
      /* Store any relevant client information here. */
      break;
    case ENET_EVENT_TYPE_RECEIVE:
      /* Clean up the packet now that we're done using it. */
      printf("got a packet!\n");
      on_receive_packet(&event);
      enet_packet_destroy(event.packet);

      break;

    case ENET_EVENT_TYPE_DISCONNECT:
      /* Reset the peer's client information. */
      event.peer->data = NULL;
    }
  }

  // Send all outgoing messages
  Msg out_mess;
  int n_received = 0;
  while (net_out_queue.try_dequeue(out_mess)) {
    send_node_msg(out_mess);
    n_received++;

    if (n_received > 100)
      break;
  }

  // Put incoming messages into the correct mailboxes
  std::map<int, std::vector<Msg>> sort_queue;
  while (!net_in_queue.empty()) {
    auto msg_front = net_in_queue.front();
    sort_queue[msg_front.vat_id].push_back(msg_front);
    net_in_queue.pop();
  }
  Vat *vat_node;
  while (net_vats.try_dequeue(vat_node)) {
    if (sort_queue.find(vat_node->id) != sort_queue.end()) {
      for (auto zz: sort_queue[vat_node->id]) {
        vat_node->messages.push(zz);
      }
    }

    queue.enqueue(vat_node);
  }

}

void on_receive_packet(ENetEvent *event) {
  std::string buf =
      std::string((char *)event->packet->data, event->packet->dataLength);
  romabuf::PleromaMessage message;
  message.ParseFromString(buf);

  // MSMC
  Msg local_m;
  local_m.entity_id = message.entity_id();
  local_m.vat_id = message.vat_id();
  local_m.node_id = message.node_id();

  local_m.src_entity_id = message.src_entity_id();
  local_m.src_vat_id = message.src_vat_id();
  local_m.src_node_id = message.src_node_id();

  local_m.promise_id = message.promise_id();
  local_m.response = message.response();

  local_m.values.push_back((ValueNode *)make_number(100));

  net_in_queue.push(local_m);
}

void on_client_connect() {}

void init_network() {
  if (enet_initialize() != 0) {
    fprintf(stderr, "An error occurred while initializing ENet.\n");
    exit(EXIT_FAILURE);
  }
}

void send_packet(ENetPeer *peer, const char *buf, int buf_len) {
  ENetPacket *packet =
      enet_packet_create(buf, buf_len, ENET_PACKET_FLAG_RELIABLE);
  enet_peer_send(peer, 0, packet);
  enet_host_flush(pnet.server);
}

void send_msg(enet_uint32 host, romabuf::PleromaMessage msg) {
  std::string buf = msg.SerializeAsString();

  send_packet(pnet.peers[host], buf.c_str(), buf.length() + 1);
}

void send_node_msg(Msg m) {
  romabuf::PleromaMessage message;
  message.set_node_id(m.node_id);
  message.set_entity_id(m.entity_id);
  message.set_vat_id(m.vat_id);

  message.set_src_node_id(m.src_node_id);
  message.set_src_entity_id(m.src_entity_id);
  message.set_src_vat_id(m.src_vat_id);

  message.set_response(m.response);

  message.set_function_id(m.function_name);
  message.set_src_function_id(m.function_name);

  message.set_promise_id(m.promise_id);
  std::string buf = message.SerializeAsString();
  send_packet(pnet.peers[pnet.node_host_map[m.node_id]], buf.c_str(),
              buf.length() + 1);
}

void setup_server() {
  ENetAddress address;
  /* Bind the server to the default localhost.     */
  /* A specific host address can be specified by   */
  /* enet_address_set_host (& address, "x.x.x.x"); */
  enet_address_set_host(&address, "0.0.0.0");
  /* Bind the server to port 1234. */
  address.port = 1234;
  pnet.server = enet_host_create(
      &address /* the address to bind the server host to */,
      32 /* allow up to 32 clients and/or outgoing connections */,
      2 /* allow up to 2 channels to be used, 0 and 1 */,
      0 /* assume any amount of incoming bandwidth */,
      0 /* assume any amount of outgoing bandwidth */);
  if (pnet.server == NULL) {
    fprintf(stderr,
            "An error occurred while trying to create an ENet server host.\n");
    exit(EXIT_FAILURE);
  }
}

ENetAddress get_address(std::string ip) {
  ENetAddress address;
  enet_address_set_host(&address, ip.c_str());
  address.port = 1234;
  return address;
}

void connect_client(ENetAddress address) {

  ENetPeer *peer = enet_host_connect(pnet.server, &address, 2, 0);
  if (peer == NULL) {
    fprintf(stderr, "No available peers for initiating an ENet connection.\n");
    exit(EXIT_FAILURE);
  }

  ENetEvent event;
  if (enet_host_service(pnet.server, &event, 5000) > 0 &&
      event.type == ENET_EVENT_TYPE_CONNECT) {
    printf("Connection succeeded.\n");
  } else {
    printf("Connection failed.\n");
    exit(EXIT_FAILURE);
  }

  pnet.peers[peer->address.host] = peer;
}
