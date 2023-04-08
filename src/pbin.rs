use crate::common::{HashMap, String, Box, Vec, vec, str};
use crate::opcodes::{Op, decode_instruction};
use crate::bin_helpers::{read_u8_sz, read_u16_sz};

pub fn load_entity_function_table(location: &mut usize, vblock: &[u8]) -> HashMap<u32, HashMap<u32, (usize, usize)>> {
    let table_size = read_u8_sz(&vblock[*location..]);
    *location += 1;

    let mut table : HashMap<u32, HashMap<u32, (usize, usize)>> = HashMap::new();

    let mut z = 0;

    while (z < table_size.1) {
        let ent_id = read_u8_sz(&vblock[*location..]);

        *location += 1;

        let func_id = read_u8_sz(&vblock[*location..]);
        *location += 1;

        let loc = read_u16_sz(&vblock[*location..]);
        *location += 2;

        let func_len = read_u16_sz(&vblock[*location..]);
        *location += 2;

        table.entry(ent_id.1.into()).or_insert(HashMap::new());
        table.get_mut(&ent_id.1.into()).unwrap().insert(func_id.1.into(), (loc.1 as usize, func_len.1 as usize));

        z += 1;
    }

    table
}

pub fn disassemble(binary_code: &Vec<u8>) {
    println!("Disassembly:");
    let mut x = 0;
    let table = load_entity_function_table(&mut x, &binary_code[..]);

    for (eid, funcs) in &table {
        println!("EID: {}", eid);
        for (fid, (loc, sz)) in &table[&eid] {
            println!("\tFID (sz: {}): {}", sz, *fid);
            x = *loc;
            let loc_end = *sz + *loc;
            while x < loc_end {
                let inst = decode_instruction(x, &binary_code[..]);
                println!("\t\t{:?}", inst.1);
                x = inst.0;
            }
            println!();
        }
    }
}
