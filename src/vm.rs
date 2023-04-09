use crate::ast;
use crate::ast::Hvalue;
use crate::common::{vec, Box, HashMap, String, Vec};
use crate::vm_core;
use core;

use crate::opcodes::{decode_instruction, Op};
use crate::pbin::{disassemble, load_entity_data_table, load_entity_function_table};
use crate::vm_core::{Msg, StackFrame, Vat};

pub fn run_expr(
    start_addr: usize,
    vat: &mut Vat,
    msg: &Msg,
    tx_msg: &mut crossbeam::channel::Sender<vm_core::Msg>,
    code: &Vec<u8>,
) -> Option<Hvalue> {
    let mut x = start_addr;

    let mut yield_op = false;

    loop {
        // TODO: this should never happen, need implicit return
        if x >= code.len() {
            //panic!("Code overrun: x was {}, code is len {}", x, code.len());
            break;
        }

        let last_ip = x;

        let (q, inst) = decode_instruction(x, &code[..]);

        x = q;

        match inst {
            Op::Push(a0) => {
                vat.op_stack.push(a0);
            }
            Op::Add => {
                let (a0, a1) = (vat.op_stack.pop().unwrap(), vat.op_stack.pop().unwrap());
                if let (Hvalue::Hu8(b0), Hvalue::Hu8(b1)) =
                    (a0.clone(), a1.clone())
                {
                    let res = b0 + b1;
                    println!("Calling add {} + {} : {}", b0, b1, res);
                    vat.op_stack.push(Hvalue::Hu8(res));
                } else {
                    println!("Instruction: {:?}", inst);
                    println!("a0: {:?}, a1: {:?}", a0, a1);
                    println!("Vat state: {:?}", vat);
                    panic!();
                }
            }
            Op::Sub => {
                if let (Hvalue::Hu8(a0), Hvalue::Hu8(a1)) =
                    (vat.op_stack.pop().unwrap(), vat.op_stack.pop().unwrap())
                {
                    let res = a0 - a1;
                    vat.op_stack.push(Hvalue::Hu8(res));
                } else {
                    panic!();
                }
            }
            Op::Mul => {
                if let (Hvalue::Hu8(a0), Hvalue::Hu8(a1)) =
                    (vat.op_stack.pop().unwrap(), vat.op_stack.pop().unwrap())
                {
                    let res = a0 * a1;
                    vat.op_stack.push(Hvalue::Hu8(res));
                } else {
                    panic!();
                }
            }
            Op::Div => {
                if let (Hvalue::Hu8(a0), Hvalue::Hu8(a1)) =
                    (vat.op_stack.pop().unwrap(), vat.op_stack.pop().unwrap())
                {
                    let res = a0 / a1;
                    vat.op_stack.push(Hvalue::Hu8(res));
                } else {
                    panic!();
                }
            }
            Op::Lload(s0) => {
                let local_var = vat.load_local(&s0);
                vat.op_stack.push(local_var);
            }
            Op::Lstore(s0) => {
                let store_val = vat.op_stack.pop().unwrap();
                if let Hvalue::Promise(promise_id) = store_val {
                    vat.promise_stack.get_mut(&promise_id).unwrap().var_names.push(s0.clone());
                }
                vat.store_local(&s0, &store_val);
            }
            Op::Eload(s0) => {
                let mut target_entity = vat.entities.get_mut(&msg.dst_address.entity_id).unwrap();
                let local_var = target_entity.data[&s0].clone();
                vat.op_stack.push(local_var);
            }
            Op::Estore(s0) => {
                let mut target_entity = vat.entities.get_mut(&msg.dst_address.entity_id).unwrap();
                let store_val = vat.op_stack.pop().unwrap();
                target_entity.data.insert(s0, store_val);
            }
            Op::Message(a0) => {
                println!("message!!!!");
                let msg = vm_core::Msg {
                    src_address: msg.dst_address,
                    dst_address: vm_core::EntityAddress::new(0, 0, 0),

                    contents: vm_core::MsgContents::Request {
                        args: Vec::new(),
                        // TODO: u64
                        function_id: a0 as u32,
                        function_name: String::from("main"),
                        src_promise: Some(0),
                    },
                };

                tx_msg.send(msg).unwrap();
                let next_prom_id = vat.promise_stack.len() as u8;
                vat.promise_stack.insert(next_prom_id, vm_core::Promise::new());
                vat.op_stack.push(Hvalue::Promise(next_prom_id));
            }
            Op::Await => {
                // If promise is resolved, run code, else push onto on_resolve
                let op = vat.op_stack.pop().unwrap();
                if let Hvalue::Promise(target_promise) = op {
                    if vat.promise_stack[&target_promise].resolved {
                        println!("Already resolved!");
                    } else {
                        println!("Insert promise handler!");
                        vat.promise_stack
                            .get_mut(&target_promise)
                            .unwrap()
                            .save_point = (vat.op_stack.clone(), vat.call_stack.clone());
                        vat.promise_stack
                            .get_mut(&target_promise)
                            .unwrap()
                            .on_resolve
                            .push(x);
                    }
                }
                yield_op = true;
                break;
            }
            Op::Ret => {
                //let ret_val = vat.op_stack.pop().unwrap();
                let sf = vat.call_stack.pop().unwrap();

                if let Some(ret_addr) = sf.return_address {
                    x = ret_addr as usize;
                } else {
                    break;
                }
            }
            Op::ForeignCall(a0) => {
                // TODO: if kernel is recompiled, this won't point to the right memory address - need to create a table for sys modules
                let ptr = a0 as *const ();
                // TODO: create a type FFI for this
                let mut target_entity = vat.entities.get_mut(&msg.dst_address.entity_id).unwrap();
                let foreign_func: vm_core::SystemFunction = unsafe { core::mem::transmute(ptr) };
                let res = foreign_func(target_entity, Hvalue::None);

                println!("Data {:?}", target_entity.data);
                //let res = target_entity.def.foreign_functions[&a0](ast::Hvalue::None);
                vat.op_stack.push(res);
            }
            Op::Nop => {}
            _ => panic!(),
        }
    }

    if vat.op_stack.len() > 1 {
        println!("{:?}", vat.op_stack);
        panic!(
            "Invalid program, left with {} operands on stack",
            vat.op_stack.len()
        );
    }

    if yield_op {
        return None;
    }

    if vat.op_stack.is_empty() {
        Some(Hvalue::None)
    } else {
        Some(vat.op_stack.pop().unwrap())
    }
}

pub fn run_msg(
    code: &Vec<u8>,
    vat: &mut Vat,
    msg: &Msg,
    tx_msg: &mut crossbeam::channel::Sender<vm_core::Msg>,
) -> Option<Msg> {
    println!("Running message: {:?}", msg);

    let mut out_msg: Option<Msg> = None;

    match &msg.contents {
        vm_core::MsgContents::Request {
            args,
            function_id,
            function_name,
            src_promise,
        } => {
            let mut z = 0;
            let data_table = load_entity_data_table(&mut z, &code[..]);
            let q = z.clone();
            z = 0;
            let table = load_entity_function_table(&mut z, &code[q..]);

            vat.call_stack.push(StackFrame {
                locals: HashMap::new(),
                return_address: None,
            });

            z = table[&0][&function_id].0 as usize;

            let res = run_expr(z, vat, msg, tx_msg, code);

            out_msg = Some(Msg {
                src_address: msg.dst_address,
                dst_address: msg.src_address,
                contents: vm_core::MsgContents::Response {
                    result: res.unwrap(),
                    dst_promise: *src_promise,
                },
            });
        }
        vm_core::MsgContents::Response {
            result,
            dst_promise,
        } => {
            if let (promise_id) = dst_promise {
                // We want to execute from top of stack down
                let fix_id = promise_id.unwrap() as u8;
                let mut promise;
                {
                    let prom = &mut vat.promise_stack.get_mut(&fix_id).unwrap();
                    promise = prom.clone();
                }

                {
                    promise.resolved = true;
                }

                let resolutions = promise.on_resolve.clone();

                for i in resolutions {
                    vat.op_stack = promise.save_point.0.clone();
                    vat.call_stack = promise.save_point.1.clone();

                    vat.store_local(&promise.var_names[0], result);
                    run_expr(i, vat, msg, tx_msg, code);
                }
            }
        }
        vm_core::MsgContents::BigBang {
            args,
            function_id,
            function_name,
        } => {
            let mut z = 0;
            let data_table = load_entity_data_table(&mut z, &code[..]);
            let q = z.clone();
            z = 0;
            let table = load_entity_function_table(&mut z, &code[q..]);

            vat.call_stack.push(StackFrame {
                locals: HashMap::new(),
                return_address: None,
            });

            z = table[&0][&function_id].0 as usize;

            run_expr(z, vat, msg, tx_msg, code);
        }
    }

    out_msg
}
