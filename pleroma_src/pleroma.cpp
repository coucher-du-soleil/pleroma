#include "pleroma.h"
#include "general_util.h"
#include "hylic.h"
#include "hylic_ast.h"
#include "hylic_eval.h"
#include <locale>
#include <map>
#include <queue>
#include <stdexcept>
#include <stdio.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <thread>

#include "hylic_typesolver.h"
#include "netcode.h"
#include "core/kernel.h"
#include "blockingconcurrentqueue.h"
#include "node_config.h"
#include "args.h"

#include "other.h"
#include "system.h"

//const auto processor_count = std::thread::hardware_concurrency();
const auto processor_count = 1;
const int MAX_STEPS = 3;

PleromaNode *this_pleroma_node;

moodycamel::BlockingConcurrentQueue<Vat*> queue;

Msg create_response(Msg msg_in, AstNode *return_val) {
  Msg response_m;
  response_m.response = true;

  response_m.entity_id = msg_in.src_entity_id;
  response_m.vat_id = msg_in.src_vat_id;
  response_m.node_id = msg_in.src_node_id;

  response_m.src_entity_id = msg_in.entity_id;
  response_m.src_vat_id = msg_in.vat_id;
  response_m.src_node_id = msg_in.node_id;
  response_m.promise_id = msg_in.promise_id;

  response_m.function_name = msg_in.function_name;

  if (return_val->type == AstNodeType::NumberNode || return_val->type == AstNodeType::StringNode || return_val->type == AstNodeType::EntityRefNode) {
    response_m.values.push_back((ValueNode*)return_val);
  }

  return response_m;
}

void process_vq() {
  while (true) {
    Vat* our_vat;
    queue.wait_dequeue(our_vat);

    // Take a number of steps
    for (int k = 0; k < 1; ++k) {

      while (!our_vat->messages.empty()) {
        Msg m = our_vat->messages.front();
        our_vat->messages.pop();
        //print_msg(&m);

        try {
          auto find_entity = our_vat->entities.find(m.entity_id);
          assert(find_entity != our_vat->entities.end());
          Entity* target_entity = find_entity->second;

          EvalContext context;
          start_context(&context, this_pleroma_node, our_vat, target_entity->entity_def->module, target_entity);

          // Return vs call
          if (m.response) {
            // If we didn't setup a promise to resolve, then ignore the result
            if (our_vat->promises.find(m.promise_id) != our_vat->promises.end()) {
              our_vat->promises[m.promise_id].results = m.values;
              //printf("Resolving prom %d\n", m.promise_id);
              if (our_vat->promises[m.promise_id].callback) {
                eval_promise_local(&context, our_vat->entities.find(m.entity_id)->second, &our_vat->promises[m.promise_id]);
              }

              if (our_vat->promises[m.promise_id].return_msg) {
                Msg response_m = create_response(our_vat->promises[m.promise_id].msg, our_vat->promises[m.promise_id].results[0]);
                if (m.function_name != "main") {
                  printf("Got return message, sending %s!!\n", ast_type_to_string(our_vat->promises[m.promise_id].results[0]->type).c_str());
                  auto ref_res = our_vat->promises[m.promise_id].results[0];
                  if (ref_res->type == AstNodeType::EntityRefNode) {
                    auto et = (EntityRefNode*) ref_res;
                    printf("Result was %d %d %d\n", et->node_id, et->vat_id, et->entity_id);
                  }
                  our_vat->out_messages.push(response_m);
                }
              }

              // Get return value here, check if we return a promise node, if we do then we need to connect the two
            }
          } else {
            std::vector<AstNode *> args;

            for (int zz = 0; zz < m.values.size(); ++zz) {
              args.push_back(m.values[zz]);
            }

            // If the result is a promise, setup promise with callback being the real return, and don't send message
            auto result = eval_func_local(&context, target_entity, m.function_name, args);
            //print_msg(&m);
            //printf("%s\n", ast_type_to_string(result->type).c_str());
            if (result->type == AstNodeType::PromiseNode) {
              PromiseNode* prom = (PromiseNode*) result;
              our_vat->promises[prom->promise_id].return_msg = true;
              our_vat->promises[prom->promise_id].msg = m;
            } else {
              // All return values are singular - we use tuples to represent
              // multiple return values
              // FIXME might not work if we handle tuples differently
              Msg response_m = create_response(m, result);

              // Main cannot be called by any function except ours, move this logic into typechecker
              if (m.function_name != "main") {
                our_vat->out_messages.push(response_m);
              }
            }
          }
        } catch (PleromaException &e) {
          printf("PleromaException: %s\n", e.what());
          printf("Calling message: \n");
          print_msg(&m);
          throw;
        }
      }

      while (!our_vat->out_messages.empty()) {
        Msg m = our_vat->out_messages.front();
        our_vat->out_messages.pop();
        //print_msg(&m);

        // If we're communicating on the same node, we don't have to use the router
        if (m.node_id == this_pleroma_node->node_id && m.vat_id == our_vat->id) {
          our_vat->messages.push(m);
        } else {
          net_out_queue.enqueue(m);
        }
      }

      //sleep(1);

      our_vat->run_n++;
    }

    net_vats.enqueue(our_vat);

  }
}

void start_program(std::string program_name, std::string ent_name) {
  Msg m;
  m.node_id = monad_ref->node_id;
  m.vat_id = monad_ref->vat_id;
  m.entity_id = monad_ref->entity_id;
  m.function_name = "start-program";

  m.values.push_back((StringNode*)make_string(program_name));
  m.values.push_back((StringNode *)make_string(ent_name));

  m.src_entity_id = -1;
  m.src_node_id = -1;
  m.src_vat_id = -1;
  m.promise_id = -1;

  net_out_queue.enqueue(m);
}

void inoculate_pleroma(HylicModule *ukernel, std::string ent0) {

  EntityDef *ent0_def = (EntityDef *)ukernel->entity_defs[ent0];

  Vat* og_vat = new Vat;
  og_vat->id = 0;
  queue.enqueue(og_vat);
  this_pleroma_node->vat_id_base++;

  EvalContext context;
  start_context(&context, this_pleroma_node, og_vat, ukernel, nullptr);

  Entity *ent = create_entity(&context, ent0_def, false);
  ent->module_scope = ukernel;

  monad_ref = (EntityRefNode*)make_entity_ref(ent->address.node_id, ent->address.vat_id, ent->address.entity_id);
  //printf("%d %d %d\n", monad_ref->node_id, monad_ref->vat_id, monad_ref->entity_id);

  og_vat->entities[0] = ent;

  Msg m;
  m.entity_id = 0;
  m.function_name = "hello";
  m.node_id = 0;

  m.src_entity_id = -1;
  m.src_node_id = -1;
  m.src_vat_id = -1;
  m.promise_id = -1;

  m.values.push_back((ValueNode *)make_number(0));

  og_vat->messages.push(m);
}

EntityAddress start_system_program(HylicModule *ukernel, std::string ent0) {

  EntityDef *ent0_def = (EntityDef *)ukernel->entity_defs[ent0];

  Vat *og_vat = new Vat;
  og_vat->id = this_pleroma_node->vat_id_base;
  queue.enqueue(og_vat);
  this_pleroma_node->vat_id_base++;

  EvalContext context;
  start_context(&context, this_pleroma_node, og_vat, ukernel, nullptr);

  Entity *ent = create_entity(&context, ent0_def, false);
  ent->module_scope = ukernel;

  og_vat->entities[0] = ent;

  return ent->address;
}

struct ConnectionInfo {
  std::string host_ip;
  int host_port;

  std::string first_contact_ip;
  int first_contact_port;
};

void start_pleroma(PleromaArgs pleroma_args) {
  dbp(log_debug, "Reading node config [pleroma.json]...");

  this_pleroma_node = read_node_config(pleroma_args.config_path);
  add_new_pnode(this_pleroma_node);

  load_kernel();

  auto monad_mod = load_system_module(SystemModule::Monad);

  if (pleroma_args.remote_hostname == "") {
    dbp(log_debug, "Inoculating Pleroma [Monad]...");
    inoculate_pleroma(monad_mod, "Monad");
    dbp(log_debug, "Successfully inoculated.");
  }

  dbp(log_debug, "Initializing host [%s : %d]...", pleroma_args.local_hostname.c_str(), pleroma_args.local_port);
  init_network();
  setup_server(pleroma_args.local_hostname, pleroma_args.local_port);
  dbp(log_debug, "Host initialized");

  auto ent_add = start_system_program(monad_mod, "NodeMan");
  this_pleroma_node->nodeman_addr = ent_add;

  if (pleroma_args.remote_hostname != "") {
    dbp(log_info, "Connecting to network [%s : %d]...", pleroma_args.remote_hostname.c_str(), pleroma_args.remote_port);
    connect_to_cluster(mk_netaddr(pleroma_args.remote_hostname, pleroma_args.remote_port));
    dbp(log_info, "Successfully connected");

  }

  load_software();

  start_program("helloworld", "UserProgram");

  std::thread burners[processor_count];

  dbp(log_debug, "Starting %d burner processes...", processor_count);
  for (int k = 0; k < processor_count; ++k) {
    burners[k] = std::thread(process_vq);
  }

  dbp(log_debug, "Starting net loop...");
  while (true) {
    net_loop();
  }

  dbp(log_debug, "Net loop finished, joining all processes");
  for (int k = 0; k < processor_count; ++k) {
    burners[k].join();
  }

  dbp(log_info, "Burners joined, exiting.");
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");

  PleromaArgs pargs = parse_args(argc, argv);

  std::string debug_pargs = "Using the following options to start Pleroma: \n";
  debug_pargs += "\tLocal host: " + pargs.local_hostname + "\n";
  debug_pargs += "\tLocal port: " + std::to_string(pargs.local_port) + "\n";

  if (pargs.remote_hostname == "") {
    debug_pargs += "\tRemote host: none\n";
  } else {
    debug_pargs += "\tRemote host: " + pargs.remote_hostname + "\n";
    debug_pargs += "\tRemote port: " + std::to_string(pargs.remote_port) + "\n";
  }

  debug_pargs += "\tConfig path: " + pargs.config_path + "\n";

  debug_pargs += "\tStart program: " + pargs.program_path + "\n";
  debug_pargs += "\tEntity: " + pargs.entity_name + "\n";

  dbp(log_debug, debug_pargs.c_str());

  if (pargs.command == PCommand::Start) {
    start_pleroma(pargs);
  } else if (std::string(argv[1]) == "test") {
    std::string target_file = argv[2];
    load_file("test", target_file);
    exit(0);
  } else {
    exit(1);
  }

}
