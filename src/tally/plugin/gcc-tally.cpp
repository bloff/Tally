/// GCC plugin source for Tally. It instruments GIMPLE basic blocks with
/// r15-based tally accounting and optional context-switch branches.
///
/// The gcc-tally library serves to instrument gcc-compiled code with instruction counters.
/// These counters use a machine register (r15 in x86_64) to keep a rough 'tally' of the number of GIMPLE instructions ran.

/// More precisely, each GIMPLE instruction is given a certain 'cost', and at the end of each GIMPLE basic-block,
/// the cost of the instructions in that block is deduced from the register.

/// TODO:
/// Optionally, the register is then compared to zero, and if the value is <= 0, a jump is made to a certain label,
/// with the continuation value (IP of the next non-instrumentation program instruction) placed on top of the stack.
/// This is controlled via the -fplugin-arg-gcc-tally-jump=yes/no argument (default is yes)

/// TODO:
/// One may opt to allow or forbid asm instructions other than the ones inserted by the plugin itself.
/// This is controlled via the -fplugin-arg-gcc-tally-forbid-asm=yes/no argument (default is yes)
/// PROBLEM: This is not enough to avoid accessing the tally register (because C has global register variables).


/// # Usage
/// To use the plugin properly one compiles code with the options `-ffixed-r15 -fplugin=path/to/gcc-tally.so -fplugin-arg-gcc-tally-jump=yes/no -fplugin-arg-gcc-tally-forbid-asm=yes/no`


/// # Mode of Operation
/// We introduce a new optimization pass after every GIMPLE optimization pass (but before the remaining low-level passes).
/// We then scan every gimple basic-block, assumming that there is no branching happening within each block. (This will fail to be
///


#include <iostream>
#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree-pass.h>
#include <context.h>
#include <tree.h>
#include <cfg.h>
#include <function.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <gimple-pretty-print.h>
#include <gimplify.h>
#include <basic-block.h>
#include <map>
#include <vector>
#include <line-map.h>
#include <time.h>


//maybe 2 diferent files, one as plugin configs that has the structs and helper functions
//and another with the class gcc plugin, the actual instrumentation

/*
flags.h contains the compile-time options implemented in gcctally providing diferent asm code instrumentation 
*/
#include "flags.h"


int plugin_is_GPL_compatible = 1;

static struct plugin_info myplugin_info =
        {
                .version = "1",
                .help = "Not yet...",
        };


static struct plugin_gcc_version myplugin_ver =
        {
                .basever = "6",
        };


const pass_data my_gimple_pass_data =
        {
                GIMPLE_PASS,    // opt type name
                "GCC Tally Plugin",  // name
                OPTGROUP_NONE,  // optinfo_flags
                TV_NONE,        // tv_id
                PROP_cfg,       // properties_required
                0,              // properties_provided
                0,              // properties_destroyed
                0,              // todo_flags_start
                0,              // todo_flags_finish
        };

std::map<int, const char*> gimple_code_names;

/*
gimple stuff? not sure, have to ask later TODO
*/
void fill_gimple_code_names() {
#define DEFGSCODE(SYM, STRING, STRUCT)	gimple_code_names[SYM] = STRING;
#include "gimple.def"
#undef DEFGSCODE
}

//not used
//std::vector<void *> allocated_stuff;

/*
generate unique label numbers 
*/
int next_label_number() {
    static int label_number = (int)clock();
    label_number++;
    return label_number;

}

/**
 * @brief This optimization pass instruments each GIMPLE block with the purpose of counting how many machine instructions have been executed in the block.
 *
 *
 */
class GCCTallyGimplePass : public gimple_opt_pass
{
public:
    GCCTallyGimplePass(gcc::context* ctxt)
            : gimple_opt_pass(my_gimple_pass_data, ctxt)
    {}

protected:
    /**
     * Pretty-prints the GIMPLE basic-blocks associated with the given function.
     *
     * @param fun The function whose basic-blocks are to be printed.
     */

    /*
        easy place to change the current tally value from each stmt type
    */
    int get_tally_val(gimple* stmt){
        int code = stmt->code;

        switch(code){
            case GIMPLE_ASM:
                return 1;
            case GIMPLE_ASSIGN:
                return gimple_num_ops(stmt) - 1;
            case GIMPLE_LABEL:
                return 0; //insted of 1
            case GIMPLE_CALL:
                return 2 + 2 * gimple_call_num_args(stmt);
            case GIMPLE_COND:
                return 1;
            case GIMPLE_SWITCH:
                //use gimple_num_labels as its the numbers of comps it has to do in worst case
                return 3 * gimple_num_ops(stmt);
                //at this time gimple_switch does not do anything
            case GIMPLE_RETURN:
                return 1;
            case GIMPLE_PREDICT:
                return 0;
            default:
                printf("GCC-tally found a GIMPLE instruction-type (%s) that it doesn't know how to handle.\n", gimple_code_names[code]);
                fflush(stdout);
                abort();
        };
    }

    void print_cfg(function *fun) {
        // for block in cfg.basic_blocks:

        basic_block bb;
        gimple *stmt;
        gimple_stmt_iterator gsi;
        int block_nr = 0;

        std::map<basic_block, int> mymap;

        FOR_EACH_BB_FN(bb,fun) {
            mymap[bb] = ++block_nr;
        }

        FOR_EACH_BB_FN(bb,fun) {
            printf("BLOCK #%d\n", mymap[bb]);
            for (gsi=gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi))
            {
                stmt = gsi_stmt(gsi);
                printf("        ");
                print_gimple_stmt(stdout, stmt, 0, TDF_NONE);
                if(stmt->code != GIMPLE_ASM){
                    switch (stmt->code)
                    {
                    case GIMPLE_ASSIGN:
                        printf("\tassign\n");
                        break;
                    case GIMPLE_COND:
                        printf("\tcond\n");
                        break;
                    case GIMPLE_LABEL:
                        printf("\tlabel\n");
                        break;
                    case GIMPLE_CALL:
                        printf("\tcall\n");
                        break;
                    case GIMPLE_RETURN:
                        printf("\treturn\n");
                        break;
                    case GIMPLE_SWITCH:
                        printf("\tswitch\n");
                        break;

                    default:
                        printf("\tnot implemented\n");
                        break;
                    }
                    printf(" %d\n", gimple_num_ops(stmt));

                }

            }
       }
    
    }

    /**
     * @brief This is the main instrumentation function.
     *
     * The instrumentation works by going over every basic block. We insert some assembly code in each basic block:
     *
     * * If the basic block ends with a branch, we append the assembly code just before the branching instruction.
     * * Otherwise, we append the assembly code at the very end.
     *
     * The assembly code does the following:
     * * It decrements the r15 register by some quantity which is meant to roughly account for the number of instructions in the basic block.
     * * If r15 falls below 0, it then pushes the continuation address plus the "break value" into the stack, and jumps to  the `_minithread_break` label.
     * * The minithread library comes into the picture, saves the execution context and so on.
     *
     * @param fun The function whose basic-blocks are to be instrumented.
     */
    void insert_tally(function *fun) {
        basic_block bb, last_used_bb;
        gimple *stmt;
        gimple_stmt_iterator gsi, last_gsi;
        int block_nr = 0;
        location_t last_valid_location = UNKNOWN_LOCATION;

        int block_count = 0;

        #ifdef gcctally_branch_prediction
            std::vector<char*> block_return_code(fun->cfg->x_n_basic_blocks);
            int block_index = 0;
        #endif

        //macro function to insert asm code 
        #define INSERT_ASM(gsi, gsi_insert_instruction, stmt, code_str) {\
                gasm *asm_stmt = gimple_build_asm_vec(code_str, NULL, NULL, NULL, NULL);\
                gimple_asm_set_volatile(asm_stmt, true);\
                gimple_set_location(asm_stmt, last_valid_location);\
                gsi_insert_instruction(&(gsi), asm_stmt, GSI_SAME_STMT);\
                }

        FOR_EACH_BB_FN(bb,fun) {
            block_count++;
            int nstatements = 0;
            //last_gsi = -1l;
            last_used_bb = bb;
            for (gsi=gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi))
                nstatements++;

            
            int tally = 0;
            int i = 0;
            int code;
            // FIXME: HACK! If it doesn't find at least one valid location, we're screwed
            // FIXME: because of bug in gcc;
            // FIXME: The bug is in the source file gcc/final.c, the function final_scan_insn does not allow
            // FIXME: UNKNOWN_LOCATION in inline assembly nodes.
            // FIXME: The reason is that it dereferences the loc.file pointer
            // FIXME: - Yes, that took a while to figure out...
            for (gsi=gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi))
            {   
                i++;
                stmt = gsi_stmt(gsi);
                last_gsi = gsi;
                if ((unsigned int)gimple_location(stmt) != 0 && expand_location(gimple_location_safe(stmt)).file != 0 && expand_location(gimple_location_safe(stmt)).line != 0)
                    last_valid_location = gimple_location(stmt);

                tally += get_tally_val(stmt); 
                code = stmt->code;
                
                /*
                    simple condition to prevent using code that has a condition in the midle of a stmt block
                    probably not needed but better safe then sorry
                */
                if (i < nstatements ) {
                    //still not the end of the block so we don+t need to insert any code
                    if (code == GIMPLE_COND || code == GIMPLE_SWITCH || code == GIMPLE_RETURN) {
                        printf("GCC-tally found %s instruction in the middle of a block. This was unexpected!\n", gimple_code_names[code]);
                        fflush(stdout);
                        abort();
                    }
                } 
            }

            //if we don't need to subtract anything then we don't need to insert code
            if(tally == 0) continue;
           
            //after the loop ends gsi is in the last position
            //we want to insert the instrumented code on the second to last position as its the last one with code
            //see basic block structure on gimple docs
            gsi = gsi_last_bb(bb);
            code = gsi_stmt(gsi)->code;

            //code = gsi_stmt(last_gsi)->code;
            char *code_str;

            //label unique id
            int label_nr = next_label_number();

            //strings used in branch prediction 
            char *asm_block1, *asm_block2;

            /*
                this are the two ways an places were we can locate counter, or is in a register and the operation is very simple
                or in the case of it being an extern variable the process of subing from counter can be a lot more verboses
            */ 
            char *asm_sub_op;
            asprintf(&asm_sub_op,
                "sub $%d, %%%%r15\n\t", // subtracts the tally from r15 aka special_counter
                tally);
            
            #ifdef gcctally_branch_prediction
                /*
                    this is a fix for branching predition, we need a jump before the last block of switches
                    TODO example
                */
                if(block_count == fun->cfg->x_n_basic_blocks - 2){
                    #ifdef gcctally_DEBUG
                    printf("inserting fix now %d\n", i);
                    #endif
                    asprintf(&asm_block1, "jmp ._minithread_fix_branching_label_%d\n\t", label_nr);                            
                    asprintf(&asm_block2, "._minithread_fix_branching_label_%d:\n\t", label_nr);                            
                }else{
                    asprintf(&asm_block1, "");                            
                    asprintf(&asm_block2, "");                            
                }
                
                //change the order of instructions so we can load rax to rdx
                asprintf(&code_str,
                        "%s" // sub instructions
                        //if counter is <= 0 then we jump to the switch block
                        "jle ._minithread_switch_label_%d\n\t" //
                       //return label from the switch block
                        "._minithread_continue_label_%2$d:\n\t%s", //
                        asm_sub_op, label_nr, asm_block1 
                );

                //switch block
                asprintf(&block_return_code[block_index++],
                        //first put the continuation label on the stack, then put the break_label so que call ret and consume that position on the stack 
                        //in the context switch the first positon on the stack will be the ip on the next context switch
                        "._minithread_switch_label_%d:\n\t" //
                        "push ._minithread_continue_label_%1$d@GOTPCREL(%%%%rip)\n\t" //
                        "push _minithread_break@GOTPCREL(%%%%rip)\n\t" //
                        "ret\n\t%s", //
                        label_nr, asm_block2
                );
                
            #else
                asprintf(&code_str,
                        "%s" //
                        "jg ._minithread_continue_label_%d\n\t" // if the tally is positive, continue execution
                        "push ._minithread_continue_label_%2$d@GOTPCREL(%%%%rip)\n\t" //
                        "push _minithread_break@GOTPCREL(%%%%rip)\n\t" //
                        "ret\n\t" //
                        "._minithread_continue_label_%2$d:", // this defines the continuation IP value
                        asm_sub_op, label_nr); //parammeter field formating

            #endif

            /*
            when the last stmt is some sort of jump then we need to put our instrumented code before the jump happens so we are sure that the tally is correct and we do not skip the instrumentation
            */ 
            if (code == GIMPLE_COND || code == GIMPLE_RETURN) { 
                INSERT_ASM(gsi, gsi_insert_before, stmt, code_str)
            } else if (code == GIMPLE_SWITCH ) {
                //TODO at this time gimple_switch will throw a error counting the tally as i cannot find when thw gimple_switch label is used
            } else {
                INSERT_ASM(gsi, gsi_insert_after, stmt, code_str)
                gsi_next(&gsi);
            }
            free(code_str); 

            //memory needed 
            #ifdef gcctally_branch_prediction  
                free(asm_block1);
                free(asm_block2);
            #endif

        }
        
        #ifdef gcctally_branch_prediction
            gsi = gsi_last_bb(last_used_bb);
            stmt = gsi_stmt(gsi);

            
            std::string s = "";
            for(auto str : block_return_code) if(str != nullptr) s += std::string(str);
            for(auto str : block_return_code) free(str);

            int code = gimple_code(stmt); 
            if (code == GIMPLE_COND || code == GIMPLE_RETURN) { 
                INSERT_ASM(gsi, gsi_insert_before, stmt, s.c_str())
            } else {
                INSERT_ASM(gsi, gsi_insert_after, stmt, s.c_str())
            }
        #endif

    }


public:
    unsigned int execute(function *fun) {
        struct control_flow_graph *fun_cfg = fun->cfg;

        if (fun && fun_cfg) {
            #ifdef gcctally_DEBUG 
                printf("\n\nfunction: %s\n\n function before instrumentation:\n\n", function_name(fun));
                print_cfg(fun);
            #endif

            insert_tally(fun);
            
            #ifdef gcctally_DEBUG
                printf("\n\n function after instrumentation:\n\n");
                print_cfg(fun);
            #endif
        }
        return 0;
    }
};



extern "C" int plugin_init(struct plugin_name_args* plugin_info,
                       struct plugin_gcc_version* version)
{
    fill_gimple_code_names();
    const char * name = "tally";
    struct register_pass_info pass_info;
    pass_info.pass = new GCCTallyGimplePass(g);
    pass_info.reference_pass_name = "optimized";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    register_callback(name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
    return 0;
}
