#include <algorithm>
#include <act/passes/booleanize.h>
#include <act/iter.h>

#include "state_machine.h"

/*
ACT_CHP_COMMA = 0
ACT_CHP_SEMI = 1
ACT_CHP_SELECT = 2
ACT_CHP_SELECT_NONDET = 3
ACT_CHP_LOOP = 4
ACT_CHP_DOLOOP = 5
ACT_CHP_SKIP = 6
ACT_CHP_ASSIGN = 7
ACT_CHP_SEND = 8
ACT_CHP_RECV = 9
ACT_CHP_FUNC = 10
ACT_CHP_SEMILOOP = 11
ACT_CHP_COMMALOOP = 12
ACT_CHP_HOLE = 13
ACT_CHP_ASSIGNSELF = 14
ACT_CHP_MACRO = 15
ACT_HSE_FRAGMENTS = 16
ACT_CHP_INF_LOOP = 17
ACT_CHP_NULL = 18
*/

#define ACT_CHP_INF_LOOP 17
#define ACT_CHP_NULL 18

namespace fpga {

extern ActBooleanizePass *BOOL;
FILE *prs_list = NULL;

Condition *traverse_chp(Process*,UserDef*,act_chp_lang_t*,StateMachine*,StateMachine*,StateMachine*,Condition*,int,int);
static void visit_channel_ports (act_boolean_netlist_t*,Channel*,ActId*,int);
void get_full_id (act_boolean_netlist_t*,InstType*,ActId*,std::map<InstType*,std::vector<std::pair<ActId*,int>>>&);

void get_dyn_array_name (ActId *id, StateMachine *scope, std::string &str) {

  str += id->getName();
  if (id->arrayInfo()) {
    for (int i = 0; i < id->arrayInfo()->nDims(); i++) {
      str += " [";
      PrintExpression(id->arrayInfo()->getDeref(i), scope, str);
      str += " ]";
    }
  } 

  return;
}

StateMachine *init_state_machine(StateMachine *sm)
{
  StateMachine *csm = new StateMachine();
  csm->SetNumber(sm->GetKids());
  csm->SetParent(sm);
  if (sm->GetProc()) {
    csm->SetProcess(sm->GetProc());
  } else if (sm->GetChan()) {
    csm->SetChan(sm->GetChan());
    csm->SetDir(sm->GetDir());
  }

  return csm;
}

//Function to create first state and set it in 
//the corresponding state machine
State *new_state (int type, StateMachine *sm)
{
  State *s;
  if (sm->GetSize() == 0) {
    s = new State(type, 0, sm);
    sm->SetFirstState(s);
  } else {
    s = new State(type, sm->GetSize(), sm);
    sm->AddSize();
  }

  return s;
}

//Function to create new guard condition
Condition *new_guard_cond (Expr *e, StateMachine *sm)
{
  Condition *c = new Condition(e, sm->GetGN(), sm);
  sm->AddCondition(c);

  return c;
}

//Function to create new state condition
Condition *new_state_cond (State *s, StateMachine *sm)
{
  Condition *c = new Condition(s, sm->GetSN(), sm);
  sm->AddCondition(c);

  return c;
}

//Function to create new state condition
Condition *new_hs_cond (ActId *id, StateMachine *sm)
{
  Condition *c = new Condition(id, sm->GetCN(), sm);
  sm->AddCondition(c);

  return c;
}

//Function to create new comma conditio
Condition *new_comma_cond (Comma *comma, StateMachine *sm)
{
  Condition *c = new Condition(comma, sm->GetCCN(), sm);
  sm->AddCondition(c);

  return c;
}

//Function to create new comma condition starting with a vector
Condition *new_comma_cond_raw(int type, std::vector<Condition *> &vc, StateMachine *sm)
{
  Comma *com = new Comma();
  com->type = type;
  com->c = vc;
  Condition *c = new_comma_cond(com, sm);

  return c;
}

//Function to create new single element comma condtion
Condition *new_one_cond_comma (int type, Condition *cond, StateMachine *sm)
{
  Comma *com = new Comma();
  com->type = type;
  com->c.push_back(cond);
  Condition *c = new_comma_cond(com, sm);

  return c;
}

//Function to create new two element comma condtion
Condition *new_two_cond_comma (int type, Condition *cond1, Condition *cond2, StateMachine *sm)
{
  Condition *c;
  Comma *com = new Comma();
  com->type = type;
  if (cond1) { com->c.push_back(cond1); }
  if (cond2) { com->c.push_back(cond2); }
  c = new_comma_cond(com, sm);
  
  return c;
}

Condition *process_recv (
                        Process *proc, 
                        UserDef *ud,
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,
                        StateMachine *tsm,
                        StateMachine *psm,
                        Condition *pc,
                        int par_chp,
                        int opt
                        ) {
  Scope *scope;
  if (ud) {
    scope = ud->CurScope();
  } else {
    scope = proc->CurScope();
  }
  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);

  //Recv is a communication statement. It includes two
  //states. First is an IDLE state which waits for the
  //parent machine to switch to the corresponding state.
  //When both Recv and parent machines are in the right
  //state valid/ready signal is set high and waits for
  //the communication to complete, i.e. ready and valid
  //signal of the the same channel to be high. When 
  //communication is over (1 clock cycle) Recv machine
  //switches to the EXIT state.

  //Create initial state and corresponding condition
  State *s = new_state(ACT_CHP_RECV, sm);
  Condition *zero_s_cond = new_state_cond(s, sm);

  //Create handshaking condition
  ActId *chan_id = NULL;
  std::string chan_id_str = "";
  if (!ud) {
    chan_id = chp_lang->u.comm.chan->Canonical(scope)->toid();
  } else {
    chan_id = chp_lang->u.comm.chan;
  }
  char chan_tmp[1024];
  chan_id->sPrint(chan_tmp,1024);
  chan_id_str = chan_tmp;
  Condition *hs_compl = new_hs_cond(chan_id, sm);

  //Create communication completion condition
  Condition *commu_compl = new_two_cond_comma(0, pc, hs_compl, sm);

  //Create initial condition when both parent 
  //and child are in the right state
  Condition *init_cond = new_two_cond_comma(0, pc, zero_s_cond, sm);

  //Create initial switching condition when
  //both parent and child are in the right state
  //and communication complete
  Condition *exit_cond = new_one_cond_comma(0,commu_compl,sm);

  //Create second state aka exit state to wait
  //until parent switches its state
  State *exit_s = new_state(ACT_CHP_SKIP, sm);
  Condition *exit_s_cond = new_state_cond(exit_s, sm);

  //Add exit state to the init state
  s->AddNextStateRaw(exit_s, exit_cond);

  //Processing channel in the normal CHP
  if (!ud) {
    CHPData *d = NULL;
    act_connection *chan_con = NULL;
    chan_con = chan_id->Canonical(scope);

    ActId *var_id = chp_lang->u.comm.var;

    int data_type = tsm->GetVarType(chan_con) == 3 ? 6 : 1;

    InstType *it1;
    it1 = chan_id->Canonical(scope)->getvx()->t;
    //If variable is a structure then process separately
    //(assuming if var is user type then channel must be same)
    if (TypeFactory::isUserType(it1)) {
      d = new CHPData(data_type, proc, tsm, exit_cond,
                           init_cond, var_id, chan_id);
      if (var_id) {
        char var_id_tmp[1024];
        var_id->sPrint(var_id_tmp,1024);
        std::string var_id_str = var_id_tmp;
        tsm->test_data[var_id_str].push_back(d);
      }
      tsm->test_hs_data[chan_id_str].push_back(d);
    } else {
      Chan *ch_tmp = dynamic_cast<Chan *> (it1->BaseType());
      InstType *it2 = ch_tmp->datatype();
      if (TypeFactory::isUserType(it2->BaseType())) {
        UserDef *ud_tmp = NULL;
        std::map<InstType *,std::vector<std::pair<ActId *,int>>> chan_id_tmp;
        ud_tmp = dynamic_cast<UserDef *>(it2->BaseType());
        get_full_id(bnl,it2,chan_id,chan_id_tmp);
      
        //Add one HS condition
        d = new CHPData(data_type, proc, tsm, exit_cond,
                             init_cond, var_id, chan_id);
        tsm->test_hs_data[chan_id_str].push_back(d);
      
        if (var_id) {
          //Collecting var structure members
          std::map<InstType *,std::vector<std::pair<ActId *,int>>> var_id_tmp;
          ud_tmp = dynamic_cast<UserDef *>(it2->BaseType());
          get_full_id(bnl,it2,var_id,var_id_tmp);
      
          //For each member in the channel and variable
          //add one assigment (members stored in the same order)
          ActId *sub_var_id = NULL;
          ActId *sub_chan_id = NULL;
          auto a = var_id_tmp.begin();
          auto b = chan_id_tmp.begin();
          char sub_var_tmp[1024];
          char sub_chan_tmp[1024];
          for (auto j = 0; j < chan_id_tmp.size(); j++) {
            for (auto i = 0; i < a->second.size(); i++) {
              sub_var_id = a->second[i].first;
              sub_var_id->sPrint(sub_var_tmp,1024);
              sub_chan_id = b->second[i].first;
              sub_chan_id->sPrint(sub_chan_tmp,1024);
              d = new CHPData(data_type, proc, tsm, exit_cond,
                                    init_cond, sub_var_id, sub_chan_id);
              std::string sub_var_str = sub_var_tmp;
              std::string sub_chan_str = sub_chan_tmp;
              tsm->test_data[sub_var_str].push_back(d);
            }
            a++;
            b++;
          }
        }
      } else {
        chan_con = chan_id->Canonical(scope);
        char var_id_tmp[1024];
        std::string var_id_str = "";
        act_dynamic_var *dv;
        act_connection *var_con = NULL;
        if (var_id) {
          dv = BOOL->isDynamicRef(bnl, var_id);
          if (dv) {
            var_con = dv->id;
          } else {
            var_id->sPrint(var_id_tmp,1024);
            var_id_str = var_id_tmp;
          }
        }
        d = new CHPData(data_type, proc, tsm, exit_cond,
                                      init_cond, var_id, chan_id);
        if (var_id) {
          if (dv) {
            tsm->test_dyn_data[var_con].push_back(d);
          } else {
            tsm->test_data[var_id_str].push_back(d);
          }
        }
        tsm->test_hs_data[chan_id_str].push_back(d);
      }
    }
  //processing channel in the Channel definition
  } else {
    CHPData *d = NULL;
    ActId *var_id = chp_lang->u.comm.var;
    int data_type = 1;
    d = new CHPData(data_type, NULL, tsm, exit_cond,
                                    init_cond, var_id, chan_id);
    tsm->AddGlueData(d);
  }

  //Create return condition when parent
  //machine switches from the current state
  if (pc && par_chp != ACT_CHP_INF_LOOP) {
    Condition *npar_cond = new_one_cond_comma (2,pc,sm);
    exit_s->AddNextStateRaw(s, npar_cond);
  } else if (par_chp == ACT_CHP_INF_LOOP) {
    //Special case *[ X?x ] where RECV machine is
    //controled by itself
    exit_s->AddNextStateRaw(s, exit_s_cond);
  }

  //Terminate condition is when recv machine is in
  //the exit state
  Condition *term_cond;
  term_cond = new_one_cond_comma(1, exit_s_cond, sm);

  return term_cond;

}

Condition *process_send (
                        Process *proc, 
                        UserDef *ud,
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,
                        StateMachine *tsm,
                        StateMachine *psm,
                        Condition *pc,
                        int par_chp,
                        int opt
                        ) {
  Scope *scope;
  if (ud) {
    scope = ud->CurScope();
  } else {
    scope = proc->CurScope();
  }
  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);

  //Send is a communication statement. It includes two
  //states. First is an IDLE state which waits for the
  //parent machine to switch to the corresponding state.
  //When both send and parent machines are in the right
  //state valid/ready signal is set high and waits for
  //the communication to complete, i.e. ready and valid
  //signal of the the same channel to be high. When 
  //communication is over (1 clock cycle) send machine
  //switches to the EXIT state.

  //Create initial state and corresponding condition
  State *s = new_state(ACT_CHP_SEND, sm);
  Condition *zero_s_cond = new_state_cond(s, sm);

  //Create handshaking condition
  ActId *chan_id = NULL;
  std::string chan_id_str = "";
  if (!ud) {
    chan_id = chp_lang->u.comm.chan->Canonical(scope)->toid();
  } else {
    chan_id = chp_lang->u.comm.chan;
  }
  char chan_tmp[1024];
  chan_id->sPrint(chan_tmp,1024);
  chan_id_str = chan_tmp;
  Condition *hs_compl = new_hs_cond(chan_id, sm);

  //Create communication completion condition
  Condition *commu_compl = new_two_cond_comma(0, pc, hs_compl, sm);

  //Create initial condition when both parent 
  //and child are in the right state to set valid/ready high
  Condition *init_cond = new_two_cond_comma(0, pc, zero_s_cond, sm);

  //Create exit switching condition when
  //both parent and child are in the right state
  //and communication complete
  Condition *exit_cond = new_one_cond_comma(0,commu_compl, sm);

  //Create second state aka exit state to wait
  //until parent switches its state
  State *exit_s = new_state (ACT_CHP_SKIP, sm);
  Condition *exit_s_cond = new_state_cond(exit_s,sm);

  //Add exit state to the init state
  s->AddNextStateRaw(exit_s, exit_cond);

  //Processing channel by finding its direction
  //and bitwidth in the booleanize data structure
  //as well as declaring all undeclared variables 
  //used in the channel send list
  if (!ud) {
    CHPData *d = NULL;
    act_connection *chan_con;
    chan_con = chan_id->Canonical(scope);
  
    Expr *se = chp_lang->u.comm.e;
  
    int data_type = tsm->GetVarType(chan_con) == 3 ? 5 : 2;

    bool do_normal = true;

    if (se && se->type == E_VAR) {
      //Structure over channel
      InstType *it1 = NULL;
      it1 = chan_id->Canonical(scope)->getvx()->t;
      if (!TypeFactory::isUserType(it1)) {
        Chan *ch_tmp = dynamic_cast<Chan *> (it1->BaseType());
        InstType *it2 = ch_tmp->datatype();
        if (TypeFactory::isUserType(it2->BaseType())) {
          UserDef *ud_tmp = NULL;
          std::map<InstType *,std::vector<std::pair<ActId *,int>>> chan_id_tmp;
          ud_tmp = dynamic_cast<UserDef *>(it2->BaseType());
          get_full_id(bnl,it2,chan_id,chan_id_tmp);
    
          ActId *var_id = (ActId *)se->u.e.l;
          //Add one HS condition
          d = new CHPData(data_type, proc, tsm, exit_cond,
                                init_cond, var_id, chan_id);

          tsm->test_hs_data[chan_id_str].push_back(d);

          //Collecting var structure members
          std::map<InstType *,std::vector<std::pair<ActId *,int>>> var_id_tmp;
          ud_tmp = dynamic_cast<UserDef *>(it2->BaseType());
          get_full_id(bnl,it2,var_id,var_id_tmp);

          //For each member in the channel and variable
          //add one assigment (members stored in the same order)
          ActId *sub_var_id = NULL;
          ActId *sub_chan_id = NULL;
          auto a = var_id_tmp.begin();
          auto b = chan_id_tmp.begin();
          char sub_var_tmp[1024];
          char sub_chan_tmp[1024];
          for (auto j = 0; j < chan_id_tmp.size(); j++) {
            for (auto i = 0; i < a->second.size(); i++) {
              sub_var_id = a->second[i].first;
              sub_var_id->sPrint(sub_var_tmp,1024);
              sub_chan_id = b->second[i].first;
              sub_chan_id->sPrint(sub_chan_tmp,1024);
              d = new CHPData(7, proc, tsm, exit_cond,
                                    init_cond, sub_chan_id, sub_var_id);
              std::string sub_var_str = sub_var_tmp;
              std::string sub_chan_str = sub_chan_tmp;
              tsm->test_data[sub_chan_str].push_back(d);
            }
            a++;
            b++;
          }
          do_normal = false;
        }
      }
    }

    if (do_normal) {
      char chan_id_tmp[1024];
      chan_id->sPrint(chan_id_tmp,1024);
      chan_id_str = chan_id_tmp;
      d = new CHPData(data_type, proc, tsm, exit_cond,
                                    init_cond, chan_id, se);
      if (se) { tsm->test_data[chan_id_str].push_back(d); }
      tsm->test_hs_data[chan_id_str].push_back(d);
    }
  } else {
    CHPData *d = NULL;
    ActId *var_id = chp_lang->u.comm.var;
    int data_type = 2;
    d = new CHPData(data_type, NULL, tsm, exit_cond,
                                    init_cond, chan_id, var_id);
    tsm->AddGlueData(d);
  }
  //Create return condition when parent
  //machine switches from the current state
  if (pc && par_chp != ACT_CHP_INF_LOOP) {
    Condition *npar_cond = new_one_cond_comma(2, pc, sm);
    exit_s->AddNextStateRaw(s, npar_cond);
  } else if (par_chp == ACT_CHP_INF_LOOP) {
    exit_s->AddNextStateRaw(s, exit_s_cond);
  }

  //Terminate condition is when send machine is in
  //the exit state 
  Condition *term_cond;
  term_cond = new_one_cond_comma(1, exit_s_cond, sm);

  return term_cond;
}

Condition *process_assign (
                        Process *proc, 
                        UserDef *ud,
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,
                        StateMachine *tsm,
                        StateMachine *psm,
                        Condition *pc,
                        int par_chp,
                        int opt
                        ) {

  Scope *scope;
  if (ud) {
    scope = ud->CurScope();
  } else {
    scope = proc->CurScope();
  }
  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);

  //Assignment is a state machine with two states.
  //First is IDLE and EXECUTION combined. It waits
  //till parent is in the right state to 
  //evaluate expression and switch to the EXIT state
  //at the next clock cycle. Exit state is 
  //a termination condition

  //Create initial state and corresponding condition
  State *s = new_state(ACT_CHP_ASSIGN, sm);
  Condition *zero_s_cond = new_state_cond(s, sm);

  //Create initial condition when both parent 
  //and child are in the right state. This is when
  //the assignment takes place
  Condition *init_cond = new_two_cond_comma(0, pc, zero_s_cond, sm);

  //Create second state aka exit state to wait
  //until parent switches its state
  State *exit_s = new_state(ACT_CHP_SKIP, sm);
  Condition *exit_s_cond = new_state_cond(exit_s, sm);

  //Add exit state to the init state
  s->AddNextStateRaw(exit_s, init_cond);

  //Processing assigned variable as well as all other
  //variables used in the assigned expression. This is 
  //necessary for the declaration section is Verilog
  //module
  CHPData *d = NULL;
  ActId *var_id = chp_lang->u.assign.id;
  Expr *e = chp_lang->u.assign.e;

  act_connection *var_con = NULL;
  act_dynamic_var_t *dv = NULL;
  if (ud) {
    var_con = var_id->Canonical(scope);
  } else {
    dv = BOOL->isDynamicRef(bnl, var_id);
    if (dv) {
      var_con = dv->id;
    } else {
      var_con = var_id->Canonical(scope);
    }
  }

  std::string self = "self";
  char var_id_tmp[1024];
  std::string var_id_str = "";
  var_id->sPrint(var_id_tmp,1024);
  if (self == var_id_tmp) {
    var_id = new ActId("recv_data",NULL);
    var_id_str = "recv_data";
  } else {
    if (dv) {
      get_dyn_array_name(var_con->toid(), tsm, var_id_str);
    }
    var_id_str = var_id_tmp;
  }

  d = new CHPData(0, proc, tsm, init_cond, NULL, var_id, e);
  if (dv) {
    tsm->test_dyn_data[var_con].push_back(d);
  } else {
    tsm->test_data[var_id_str].push_back(d);
  }

  //Create termination condition using an exit state condition
  Condition *term_cond;
  term_cond = new_one_cond_comma (1, exit_s_cond, sm);
  //TODO: This creates combinational loop. Need to find a better way.
  //if (opt >= 2) {
  //  //PC & (S0 | S1)
  //  Condition *tmp;
  //  tmp = new_two_cond_comma (1, zero_s_cond, exit_s_cond, sm);
  //  term_cond = new_two_cond_comma (0, tmp, pc, sm);
  //} else {
  //  term_cond = new_one_cond_comma (1, exit_s_cond, sm);
  //}

  //Create a return condition as a negation of the parent
  //condition or as a more optimized version return back
  //right after switching to exit state
  if (pc) {
    if (par_chp == ACT_CHP_LOOP |
        par_chp == ACT_CHP_SELECT | par_chp == ACT_CHP_SELECT_NONDET
        & opt == 2) {
      exit_s->AddNextStateRaw(s, term_cond);
    } else {
      Condition *npar_cond = new_one_cond_comma(2,pc,sm);
      exit_s->AddNextStateRaw(s, npar_cond);
    }
  } else if (par_chp == ACT_CHP_INF_LOOP) {
    exit_s->AddNextStateRaw(s, term_cond);
  }

  return term_cond;
}

Condition *process_loop (
                        Process *proc, 
                        UserDef *ud,
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,
                        StateMachine *tsm,
                        StateMachine *psm,
                        Condition *pc,
                        int par_chp,
                        int opt
                        ) {

  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);
  Condition *tmp = NULL;

  //Loop type statement keeps executing while at least one guard
  //is true. Termination condition is AND of all guards being false.
  std::vector<Condition *> vc;
  std::vector<Condition *> vt;

  int inf_flag = 0;
  if (!chp_lang->u.gc->g) { inf_flag = 1; }

  if (inf_flag == 0) {
    //Create initial state and corresponding state condition
    State *s = new_state(ACT_CHP_LOOP, sm);
    Condition *zero_s_cond = new_state_cond(s, sm);
  
    Condition *child_cond = NULL;
    //Traverse all branches
    for (auto gg = chp_lang->u.gc; gg; gg = gg->next) {
  
      Condition *g = NULL;
      State *ss = NULL;
  
      //If loop is infinite there is no guard and guard
      //condition is replaced with the zero state condition
      if (gg->s->type != ACT_CHP_SKIP && 
          gg->s->type != ACT_CHP_FUNC) {
        g = new_guard_cond(gg->g, sm);
      } else {
        continue;
      }
      vt.push_back(g);
  
      ss = new_state(gg->s->type, sm);
  
      //If loop is not infinite use guard as a part of the
      ///switching condition
      //If there is no parent condition then using zero state
      //and corresponding guard is enough
      vc.push_back(zero_s_cond);
      if (g != zero_s_cond) { vc.push_back(g); }
      if (pc) { vc.push_back(pc); }
      Condition *par_cond = new_comma_cond_raw(0, vc, sm);
      s->AddNextStateRaw(ss, par_cond);
      vc.clear();
  
      //Create child condition
      child_cond = new_state_cond(ss,sm);
  
      //Traverse lower levels of CHP hierarchy
      StateMachine *csm;
      csm = init_state_machine(sm);

      tmp = traverse_chp(proc, ud, gg->s, csm, tsm, sm, child_cond, ACT_CHP_LOOP, opt);
  
      if (tmp) {
        sm->AddKid(csm);
        //Create iteration condition which is a termination
        //condition of the branch statement
        Condition *loop_c = new_one_cond_comma(0, tmp, sm);
        ss->AddNextStateRaw(s,loop_c);
      } 
    }
    vc.clear();
  
    //create condition for all negative guards
    Condition *nguard_cond = new_comma_cond_raw(2, vt, sm);
  
    //create general loop termination condition
    //when loop is in the zero state and all guards
    //are false
    vc.push_back(zero_s_cond);
    vc.push_back(nguard_cond);
    if (pc) { vc.push_back(pc); }
    Condition *exit_cond = new_comma_cond_raw(0, vc, sm);
  
    //Create exit state
    State *exit_s = new_state(ACT_CHP_SKIP, sm);
    Condition *exit_s_cond = new_state_cond(exit_s, sm);
    s->AddNextStateRaw(exit_s, exit_cond);
  
    //Create condition to return to the zero state
    //If parent condition exists then its negation is the condition
    //If loop is infinite then exit condition is the condition, 
    //however it is created just for the consistency of the model
    //and never actually used.
    if (pc) {
      Condition *npar_cond;
      npar_cond = new_one_cond_comma(2, pc, sm);
      exit_s->AddNextStateRaw(s,npar_cond);
    }
  
    //Use exit state as a termination condition
    Condition *term_cond = new_one_cond_comma(1,exit_s_cond,sm);
  
    return term_cond;
  } else {
    return traverse_chp(proc, ud, chp_lang->u.gc->s, sm, tsm, psm, pc, ACT_CHP_INF_LOOP, opt);
  }

}

Condition *process_do_loop (
                        Process *proc, 
                        UserDef *ud,
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,
                        StateMachine *tsm,
                        StateMachine *psm,
                        Condition *pc,
                        int par_chp,
                        int opt
                        ) {

  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);

  //Do loop is a contruction which guarantees at least one execution
  //of a branch statement. Do loop has only one guard and so only one
  //branch
  //!!! No support for an infinite DO LOOP since it doesn't make sense
  //and one can use regular loop
  std::vector<Condition *> vc;

  //Create execution state and corresponding condition
  State *zero_s = new_state(ACT_CHP_DOLOOP, sm);
  Condition *zero_s_cond = new_state_cond(zero_s, sm);

  Condition *child_cond = NULL;
  
  act_chp_gc *gg = chp_lang->u.gc;

  //First process statement part
  //Create child condition with parent condition and zero state
  vc.push_back(zero_s_cond);
  if (pc) {
    vc.push_back(pc);
  }
  child_cond = new_comma_cond_raw(0,vc,sm);

  //Traverse lower levels of CHP hierarchy
  StateMachine *csm;
  csm = init_state_machine(sm);

  Condition *tmp;
  tmp = traverse_chp(proc, ud, gg->s, csm, tsm, sm, child_cond, ACT_CHP_DOLOOP, opt);

  vc.clear();

  if (tmp) {
    sm->AddKid(csm);
  }

  //Condition to switch to the guard evaluation state
  State *g_ev_s = new_state(gg->s->type, sm);
  Condition *g_ev_s_cond = new_state_cond(g_ev_s, sm);

  //Switching to the guard eval state
  vc.push_back(zero_s_cond);
  if (tmp) {
    vc.push_back(tmp);
  }
  Condition *geval = new_comma_cond_raw(0, vc, sm);
  zero_s->AddNextStateRaw(g_ev_s, geval);
  vc.clear();

  //Guard condition
  Condition *g = NULL;
  if (gg->s->type != ACT_CHP_SKIP && 
      gg->s->type != ACT_CHP_FUNC) {
    g = new_guard_cond(gg->g, sm);
  } else {
    return NULL;
  }

  //Switching conditing to the execution state for guard = TRUE 
  vc.push_back(g_ev_s_cond);
  vc.push_back(g);
  if (pc) { vc.push_back(pc); }
  Condition *ret_cond = new_comma_cond_raw(0, vc, sm);
  zero_s->AddNextStateRaw(zero_s, ret_cond);
  vc.clear();

  //Switching conditing to the execution state for guard = FALSE 
  //false guard condition
  Condition *nguard_cond = new_one_cond_comma(2, g, sm);

  //Switching to hte exit state condition
  vc.push_back(g_ev_s_cond);
  vc.push_back(nguard_cond);
  if (pc) { vc.push_back(pc); }
  Condition *exit_cond = new_comma_cond_raw(0, vc, sm);

  //Exit state
  State *exit_s = new_state(ACT_CHP_SKIP, sm);
  Condition *exit_s_cond = new_state_cond(exit_s, sm);
  zero_s->AddNextStateRaw(exit_s, exit_cond);
  vc.clear();

  if (pc) {
    Condition *npar_cond;
    npar_cond = new_one_cond_comma(2, pc, sm);
    g_ev_s->AddNextStateRaw(zero_s,npar_cond);
  }

  Condition *term_cond = new_one_cond_comma(1,exit_s_cond,sm);

  return term_cond;
}

Condition *process_select_nondet (
                        Process *proc, 
                        UserDef *ud,
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,
                        StateMachine *tsm,
                        StateMachine *psm,
                        Condition *pc,
                        int par_chp,
                        int opt
                        ) {

  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);
  Condition *tmp = NULL;

  //Selection is a control statement. It waits until at least
  //one guard is true and executes selected branch. Its completion 
  //is determined by the completion of the selected branch.
  std::vector<Condition *> vc;
  std::vector<Condition *> vg;

  //Create initial state (guard will be evaluated here)
  //and corresponding state condition
  State *s = new_state(ACT_CHP_SELECT_NONDET, sm);
  Condition *zero_s_cond = new_state_cond(s, sm);

  Condition *child_cond = NULL;
  Condition *guard = NULL;
  State *ss = NULL;

  //Process all selection options
  Arbiter *arb = new Arbiter();

  for (auto gg = chp_lang->u.gc; gg; gg = gg->next) {
  
    //No NULL guard is possible unlike in the DET SELECTION
    if (gg->s && gg->s->type != ACT_CHP_FUNC) {
      guard = new_guard_cond(gg->g, sm);
      guard->MkArb();
      arb->AddElement(guard);
    } else {
      continue;
    }

    ss = new_state(gg->s->type,sm);
  
    vg.push_back(zero_s_cond);
    if (pc) { vg.push_back(pc); }
    vg.push_back(guard);

    Condition *full_guard = new_comma_cond_raw(0, vg, sm);

    s->AddNextStateRaw(ss,full_guard);
    vg.clear();

    Condition *tmp_cond = NULL;
    tmp_cond = new_state_cond(ss, sm);
    vg.push_back(tmp_cond);
    if (pc) { vg.push_back(pc); }
    child_cond = new_comma_cond_raw(0, vg, sm);
    vg.clear();

    //Traverse the rest of the hierarchy
    if (gg->s->type == ACT_CHP_COMMA || 
        gg->s->type == ACT_CHP_SEMI) {
      tmp = traverse_chp(proc, ud, gg->s, sm, tsm, sm, child_cond, 
                                           ACT_CHP_SELECT_NONDET, opt);
      //sm->AddCondition(tmp);
    //If statement is skip of func then use execution state
    //as a termination condition
    } else if (gg->s->type == ACT_CHP_SKIP || 
               gg->s->type == ACT_CHP_FUNC ) {
      if (tmp_cond) {
        tmp = tmp_cond;
      } else {
        tmp = child_cond;
      }
    } else {
      StateMachine *csm = init_state_machine(sm);
      tmp = traverse_chp(proc, ud, gg->s, csm, tsm, sm, child_cond, 
                                            ACT_CHP_SELECT_NONDET, opt);
      if (tmp) {
        sm->AddKid(csm);
      } else {
        csm = NULL;
        delete csm;
      }
    }
    if (tmp) {
      vc.push_back(tmp);
    } else {
      vc.push_back(child_cond);
    }
  }

  //Create new condition to switch to the exit state
  //after execution completion. Use ORed child terminations
  //conditions
  Condition *exit_cond = new_comma_cond_raw(1, vc, sm);
  State *exit_s = new_state(ACT_CHP_SKIP, sm);
  ss->AddNextStateRaw(exit_s, exit_cond);

  Condition *exit_s_cond = new_state_cond(exit_s, sm);
  vc.push_back(exit_s_cond);

  //Create termination condition which is an exit state
  Condition *term_cond = new_one_cond_comma(1, exit_s_cond, sm);

  //Return to the initial state when parent is not in 
  //the right state
  if (pc && par_chp != ACT_CHP_INF_LOOP) {
    Condition *npar_cond = new_one_cond_comma(2, pc, sm);
    exit_s->AddNextStateRaw(s, npar_cond);
  } else if (par_chp == ACT_CHP_INF_LOOP) {
    exit_s->AddNextStateRaw(s, exit_s_cond);
  }

  tsm->AddArb(arb);

  return term_cond;

}

Condition *process_select (
                        Process *proc, 
                        UserDef *ud,
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,
                        StateMachine *tsm,
                        StateMachine *psm,
                        Condition *pc,
                        int par_chp,
                        int opt
                        ) {

  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);

  //Selection is a control statement. It waits until a guard
  //is true and executes selected branch. Its completion 
  //is determined by the completion of the selected branch.

  std::vector<Condition *> vc;
  std::vector<Condition *> vg;
  std::vector<Condition *> ve;  //else vector to keep all explicit guards

  //Create initial state (guard will be evaluated here)
  //and corresponding state condition
  State *s = new_state(ACT_CHP_SELECT, sm);
  Condition *zero_s_cond = new_state_cond(s, sm);

  Condition *child_cond = NULL;
  Condition *guard = NULL;
  State *ss = NULL;

  int else_flag = 0;

  //Process all selection options
  for (auto gg = chp_lang->u.gc; gg; gg = gg->next) {

    //If guard is not NULL
    if (gg->g) {
      guard = new_guard_cond(gg->g, sm);
      ve.push_back(guard);
    //Guard is NULL then it is an else statement
    } else {
      if (gg->s->type == ACT_CHP_FUNC) {
        char tmp_f[1024] = "log";
        if (strcmp(tmp_f, gg->s->u.func.name->s) != 0){
          fatal_error("I don't know this function");
        }
      }
      guard = NULL;
      else_flag = 1;
    }

    //If statement is not NULL
    Condition *tmp = NULL;
    if (gg->s) {
      ss = new_state(gg->s->type, sm);
    } else {
      ss = new_state(ACT_CHP_SKIP, sm);
    }
    tmp = new_state_cond(ss, sm);

    vg.push_back(zero_s_cond);
    if (pc) { vg.push_back(pc); }

    //Create conditions(full guard) to switch to the execution
    //states using initial condition and corresponding
    //guards. If guard is NULL (else) use else vector
    //equal to all guards being false as a secondary 
    //switching condition
    if (else_flag == 1) {
      if (ve.size() > 0) {
        Condition *else_guard = new_comma_cond_raw(2, ve, sm);
        vg.push_back(else_guard);
      }
    } else {
      vg.push_back(guard);
    }

    Condition *full_guard = new_comma_cond_raw(0, vg, sm);
    s->AddNextStateRaw(ss, full_guard);
    vg.clear();

    //Create child conditions using execution states
    Condition *tmp_cond = NULL;
    tmp_cond = new_state_cond(ss, sm);
    vg.push_back(tmp);
    if (pc) { vg.push_back(pc); }
    child_cond = new_comma_cond_raw(0, vg, sm);
    vg.clear();

    //Traverse the rest of the hierarchy
    if (gg->s) {
      if (gg->s->type == ACT_CHP_COMMA || 
          gg->s->type == ACT_CHP_SEMI) {
        tmp = traverse_chp(proc, ud, gg->s, sm, tsm, sm, child_cond, ACT_CHP_SELECT, opt);
      //If statement is skip or func then use execution state
      //as a termination condition
      } else if (gg->s->type == ACT_CHP_SKIP || 
                 gg->s->type == ACT_CHP_FUNC ) {
        if (tmp_cond) {
          tmp = tmp_cond;
        } else {
          tmp = child_cond;
        }
      } else {
        StateMachine *csm = init_state_machine(sm);
        tmp = traverse_chp(proc, ud, gg->s, csm, tsm, sm, child_cond, ACT_CHP_SELECT, opt);
        if (tmp) {
          sm->AddKid(csm);
        } else {
          csm = NULL;
          delete csm;
        }
      }
    } else {
      if (tmp_cond) {
        tmp = tmp_cond;
      } else {
        tmp = child_cond;
      }
    }
    if (tmp) {
      vc.push_back(tmp);
    } else {
      vc.push_back(child_cond);
    }
  }

  //Create new condition to switch to the exit state
  //after execution completion. Use ORed child terminations
  //conditions
  Condition *exit_cond = new_comma_cond_raw(1, vc, sm);
  State *exit_s = new_state(ACT_CHP_SKIP, sm);
  ss->AddNextStateRaw(exit_s, exit_cond);
  
  Condition *exit_s_cond = new_state_cond(exit_s, sm);
  vc.push_back(exit_s_cond);

  //Create termination condition which is an exit state
  Condition *term_cond = new_one_cond_comma(1, exit_s_cond, sm);

  //Return to the initial state when parent is not in 
  //the right state
  if (pc && par_chp != ACT_CHP_INF_LOOP) {
    Condition *npar_cond = new_one_cond_comma(2, pc, sm);
    exit_s->AddNextStateRaw(s, npar_cond);
  } else if (par_chp == ACT_CHP_INF_LOOP) {
    exit_s->AddNextStateRaw(s, exit_s_cond);
  }

  return term_cond;

}

Condition *process_semi (
                        Process *proc, 
                        UserDef *ud,
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,
                        StateMachine *tsm,
                        StateMachine *psm,
                        Condition *pc,
                        int par_chp,
                        int opt
                        ) {

  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);
  Condition *tmp = NULL;

  //Semi is a chain of statements. Every statement
  //execution is intiated by the termination condition
  //of its predecessor with an exception for the very
  //first statement which is initiated by the parent
  //condition. If semi is a top statement then first
  //statement in the list is a top level machine
  std::vector<Condition *> vc;

  list_t *l = NULL;
  listitem_t *li = NULL;
  act_chp_lang_t *cl = NULL;

  l = chp_lang->u.semi_comma.cmd;

  Condition *child_cond = NULL;

  int first_skip = 0;
  int first_val = 0;
  int skipped_val = 0;

  Comma *tmp_com = new Comma();
  Condition *tmp_cond = NULL;

  Comma *term_com = new Comma();
  term_com->type = 0;

  StateMachine *next_psm = psm;

  //traverse all statements separated with semicolon
  for (li = list_first(l); li; li = list_next(li)) {

    cl = (act_chp_lang_t *)list_value(li);

    //ignore skip and func statements
    if (cl->type != ACT_CHP_SKIP & cl->type != ACT_CHP_FUNC) {

      //if the first statement was skipped then pc should be used
      //as initial condition for the first non-skip statement
      if (li == list_first(l) || 
          (((act_chp_lang_t *)(list_value(list_first(l))))->type == ACT_CHP_SKIP ||
           ((act_chp_lang_t *)(list_value(list_first(l))))->type == ACT_CHP_FUNC )
	  && (first_skip == 0)) {
        first_skip = 1;
        if (par_chp == ACT_CHP_INF_LOOP) {
          tmp_com->type = 2;
          tmp_cond = new Condition (tmp_com, sm->GetCCN(), sm);
          sm->AddCondition(tmp_cond);
          vc.push_back(tmp_cond);
          if (pc) { vc.push_back(pc); }
          child_cond = new_comma_cond_raw(0, vc, sm);
        } else {
          if (pc) {
            child_cond = pc;
          } else {
            child_cond = NULL;
          }
        }
      } else {
        Comma *child_com = new Comma();
        child_com->type = 0;
        child_com->c.push_back(tmp);
        if (pc) { child_com->c.push_back(pc); }
        child_cond = new Condition(child_com, sm->GetCCN(), sm);
        sm->AddCondition(child_cond);
      }

      if (sm->IsEmpty()) {
        tmp = traverse_chp(proc, ud, cl, sm, tsm, next_psm, child_cond, ACT_CHP_SEMI, opt);
        if (!next_psm) { next_psm = sm; }
      } else {
        StateMachine *csm = init_state_machine(sm);
        tmp = traverse_chp(proc, ud, cl, csm, tsm, next_psm, child_cond, ACT_CHP_SEMI, opt);
        csm->SetNumber(sm->GetKids());
        if (tmp) {
          sm->AddKid(csm);
        } else if (cl->type == ACT_CHP_LOOP) {
          sm->AddKid(csm);
        } else {
          csm = NULL;
          delete csm;
        }
      }
      //if valid condition returned - replace old valid
      //termination condition
      if (tmp) {
        if (term_com->c.size() > 0) {
          term_com->c.pop_back();
        }
        term_com->c.push_back(tmp);
      }
    }
  }

  //Create termination condition (last valid child term condition) 
  Condition *term_cond = new Condition(term_com, sm->GetCCN(), sm);
  if ((opt >= 2 && (par_chp == ACT_CHP_LOOP)) || (par_chp == ACT_CHP_INF_LOOP)) {
    tmp_com->c.push_back(term_cond);
  }
  sm->AddCondition(term_cond);

  return term_cond;

}

Condition *process_comma (
                        Process *proc, 
                        UserDef *ud,
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,
                        StateMachine *tsm,
                        StateMachine *psm,
                        Condition *pc,
                        int par_chp,
                        int opt
                        ) {

  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);
  Condition *tmp = NULL;

  //Comma type completion means concurrent completion of 
  //all comma'ed statements. If parent condition exists
  //then execution is synchronized by pc otherwise a 
  //dummy state is created at the higher level(e.g. first 
  //statement in the SEMI chain) 
  std::pair<State *, Condition *> n;
  std::vector<Condition *> vc;

  list_t *l;
  listitem_t *li;
  act_chp_lang_t *cl;

  l = chp_lang->u.semi_comma.cmd;

  Condition *child_cond;
  if (pc) {
    child_cond = pc;
  } else {
    child_cond = NULL;
  }

  //traverse all COMMAed statements
  for (li = list_first(l); li; li = list_next(li)) {

    cl = (act_chp_lang_t *)list_value(li);

    //Ignore skip and func statements
    if (cl->type == ACT_CHP_SKIP || 
        cl->type == ACT_CHP_FUNC) {
      continue;
    }

    //if statement is SEMI then no new sm
    //is needed otherwise create new child sm
    if (sm->IsEmpty()) {
      tmp = traverse_chp(proc, ud, cl, sm, tsm, sm, child_cond, ACT_CHP_COMMA, opt);
    } else {
      StateMachine *csm = init_state_machine(sm);
      tmp = traverse_chp(proc, ud, cl, csm, tsm, sm, child_cond, ACT_CHP_COMMA, opt);
      if (tmp) {
        sm->AddKid(csm);
      } else if (cl->type == ACT_CHP_LOOP) { //Loop return NULL only in the INF case
        sm->AddKid(csm);
      } else {
        csm = NULL;
        delete csm;
      }
    }

    //add child termination condition to the termination  
    //vector
    if (tmp) {
      vc.push_back(tmp);
    }
  }

  //create general termination conditon using
  //ANDed child termination conditions, i.e.
  //child_term1 & child_term2 & ... & child_termN
  if (vc.size() > 0) {
    Condition *term_cond = new_comma_cond_raw(0, vc, sm);
    return term_cond;
  } else {
    return NULL;
  }

}

//Function to traverse CHP description and collect all 
//neccessary data to to build state machine. Function 
//returns condition type to handle cases where return 
//state might be one of the previously added states.
//For exmaple, loops or commas.
Condition *traverse_chp(Process *proc,
                        UserDef *ud, 
                        act_chp_lang_t *chp_lang, 
                        StateMachine *sm,   //current scope machine
                        StateMachine *tsm,  //top scope machine
                        StateMachine *psm,  //prev/parent machine
                        Condition *pc,      //parent init cond
                        int par_chp,        //parent chp type
                        int opt             //optimization level
                        ) {

  act_boolean_netlist_t *bnl = BOOL->getBNL(proc);
  Condition *tmp = NULL;
  switch (chp_lang->type) {
  case ACT_CHP_COMMA: {
    return process_comma(proc,ud,chp_lang,sm,tsm,psm,pc,par_chp,opt);
  }
  case ACT_CHP_SEMI: {
    return process_semi(proc,ud,chp_lang,sm,tsm,psm,pc,par_chp,opt);
  }
  case ACT_CHP_SELECT: {
    return process_select(proc,ud,chp_lang,sm,tsm,psm,pc,par_chp,opt);
  }
  case ACT_CHP_SELECT_NONDET: {
    return process_select_nondet(proc,ud,chp_lang,sm,tsm,psm,pc,par_chp,opt);
  }
  case ACT_CHP_LOOP: {
    return process_loop(proc,ud,chp_lang,sm,tsm,psm,pc,par_chp,opt);
  }
  case ACT_CHP_DOLOOP: {
    return process_do_loop(proc,ud,chp_lang,sm,tsm,psm,pc,par_chp,opt);
  }
  case ACT_CHP_SKIP: {
    return NULL;
    break;
  }
  case ACT_CHP_ASSIGN: {
    return process_assign(proc,ud,chp_lang,sm,tsm,psm,pc,par_chp,opt);
  }
  case ACT_CHP_SEND: {
    return process_send(proc,ud,chp_lang,sm,tsm,psm,pc,par_chp,opt);
  }
  case ACT_CHP_RECV: {
    return process_recv(proc,ud,chp_lang,sm,tsm,psm,pc,par_chp,opt);
  }
  case ACT_CHP_FUNC: {
    fprintf(stdout, "FUNC : not sure I know what to do with this\n");
    break;
  }
  case ACT_CHP_SEMILOOP: {
    fprintf(stdout, "SEMILOOP should be expanded\n");
    break;
  }
  case ACT_CHP_COMMALOOP: {
    fprintf(stdout, "COMMALOOP should be expanded\n");
    break;
  }
  default: {
    fprintf(stdout, "Unknown chp type: %d\n", chp_lang->type);
    break;
  }
  }
  return NULL;
}

int get_proc_level (Process *p, ActId *id) {

  int lev = -1;
  
  if (id) {
    lev = ActNamespace::Act()->getLevel (id);
  }
  if (lev == -1 && p) {
    lev = ActNamespace::Act()->getLevel (p);
  }
  if (lev == -1) {
    lev = ActNamespace::Act()->getLevel ();
  }
  
  return lev;
}

act_connection *get_conn_root (act_connection *p) {

  act_connection *ret = p;
  while (ret->parent) {
    ret = ret->parent;
  }

  return ret;  
}

void create_port(act_boolean_netlist_t *bnl, act_connection *c, int input, StateMachine *sm, int func) {

  Scope *cs = bnl->p->CurScope();

  ActId *tmp_id;
  ValueIdx *tmp_v;
  act_connection *tmp_con;
  unsigned int tmp_d = 0;
  unsigned int tmp_w = 0;
  unsigned int chan = 0;
  ihash_bucket *hb;
  int reg = 1;

  tmp_id = c->toid();
  tmp_v = tmp_id->rootVx(cs);
  tmp_con = tmp_id->Canonical(cs);
  tmp_d = input;

  hb = ihash_lookup(bnl->cH, (long)tmp_con);
  act_booleanized_var_t *bv;

  if (hb) {
    bv = (act_booleanized_var_t *)hb->v;

    if (func == 0) {
      if (bv->usedchp == 0) { return; } 
      tmp_w = bv->width;
      chan = bv->ischan;
    } else {
      if (bv->used == 0) { return; }
      tmp_w = 1;
      chan = 0;
    }

    for (auto in : sm->GetInst()) {
      for (auto pp : in->GetPorts()) {
        if (pp->GetVx() == tmp_v) {
          reg = 0;
          break;
        }
      }
    }

    InstType *it = bv->id->getvx()->t;
    Port *p;
    if (func == 0) {
      if (TypeFactory::isChanType(it->BaseType())) {
        // defchan/deftype
        if (TypeFactory::isUserType(it)) {
          p = new Port(tmp_d, tmp_w, chan, reg, tmp_v, tmp_id, tmp_con);
          p->SetOwner(sm);
          sm->AddPort(p);
        } else {
          Chan *tmp_chan = dynamic_cast<Chan*>(it->BaseType());
          auto it1 = tmp_con->getvx()->t;
          // chan(user type)
          if (TypeFactory::isUserType(tmp_chan->datatype())) {
            auto it2 = tmp_chan->datatype()->BaseType();
            UserDef *ud_tmp = dynamic_cast<UserDef *>(it2);
            std::map<InstType *,std::vector<std::pair<ActId *,int>>> port_id_tmp;
            get_full_id(bnl,tmp_chan->datatype(),tmp_id,port_id_tmp);
            Port *p = new Port(tmp_d, 0, chan, reg, tmp_v, tmp_id, tmp_con);
            p->SetOwner(sm);
            sm->AddPort(p);
            for (auto id : port_id_tmp) {
              for (auto pp : id.second) {
                tmp_id = pp.first;
                tmp_v = pp.first->rootVx(cs);
                tmp_w = pp.second;
                Port *p = new Port(tmp_d, tmp_w, chan, reg, tmp_v, tmp_id, tmp_con);
                p->SetType(1);
                p->SetOwner(sm);
                sm->AddPort(p);
              }
            }
          // chan(base type)
          } else {
            Port *p = new Port(tmp_d, tmp_w, chan, reg, tmp_v, tmp_id, tmp_con);
            p->SetOwner(sm);
            sm->AddPort(p);
          }
        }
      // non channel port
      } else {
        p = new Port(tmp_d, tmp_w, chan, reg, tmp_v, tmp_id, tmp_con);
        p->SetOwner(sm);
        sm->AddPort(p);
      }
    } else {
      p = new Port(tmp_d, tmp_w, chan, reg, tmp_v, tmp_id, tmp_con);
      p->SetOwner(sm);
      sm->AddPort(p);
    }
  }

  return;
}

//Adding process ports
void add_ports(act_boolean_netlist_t *bnl, StateMachine *sm){

  Scope *cs = bnl->p->CurScope();
  act_languages *lang = bnl->p->getlang();

  ActId *tmp_id;
  ValueIdx *tmp_v;
  act_connection *tmp_c;
  unsigned int tmp_d = 0;
  unsigned int tmp_w = 0;
  unsigned int chan = 0;
  ihash_bucket *hb;
  int reg = 1;

  int lev = get_proc_level(bnl->p, NULL);
  if (lev == ACT_MODEL_CHP && lang->getchp()) {
    for (auto i = 0; i < A_LEN(bnl->chpports); i++) {
      if (bnl->chpports[i].omit == 1) { continue; }
      if (bnl->chpports[i].c->toid()->isFragmented(bnl->cur)) { continue; }
      act_connection *c = bnl->chpports[i].c;
      int input = bnl->chpports[i].input;
      create_port(bnl, c, input, sm, 0);
    }
  } else if (lev == ACT_MODEL_PRS && lang->getprs()) {
    for (int i = 0; i < A_LEN(bnl->ports); i++) {
      if (bnl->ports[i].omit == 1) { continue; }
      act_connection *c = bnl->ports[i].c;
      int input = bnl->ports[i].input;
      create_port(bnl, c, input, sm, 1);
    }
    for (auto i = 0; i < A_LEN(bnl->used_globals); i++) {
      tmp_id = bnl->used_globals[i].c->toid();
      tmp_v = tmp_id->rootVx(cs);
      tmp_c = tmp_id->Canonical(cs);
      tmp_d = 1;
      tmp_w = 1;

      reg = 1;
      for (auto in : sm->GetInst()){
        for (auto pp : in->GetPorts()) {
          if (pp->GetVx() == tmp_v) {
            reg = 0;
            break;
          }
        }
      }

      Port *p = new Port(tmp_d, tmp_w, 0, reg, tmp_v, tmp_id, tmp_c);
      p->SetOwner(sm);

      sm->AddPort(p);
    }
  } else {
    if (Act::lang_subst) {
      for (auto i = 0; i < A_LEN(bnl->chpports); i++) {
        if (bnl->chpports[i].omit == 1) { continue; }
        if (bnl->chpports[i].c->toid()->isFragmented(bnl->cur)) { continue; }
        act_connection *c = bnl->chpports[i].c;
        int input = bnl->chpports[i].input;
        create_port(bnl, c, input, sm, 0);
      }
    }
  }

  return;
}

//Map machine instances to their origins
void map_instances(CHPProject *cp){
  int found = 0;
  for (auto pr0 = cp->Head(); pr0; pr0 = pr0->GetNext()) {
    for (auto inst : pr0->GetInst()) {
      if (inst->GetGlue() == 0) {
        for (auto pr1 = cp->Head(); pr1; pr1 = pr1->GetNext()) {
          if (inst->GetProc() == pr1->GetProc()) {
            inst->SetSM(pr1);
          }
        }
      } else {
        for (auto gp = cp->GHead(); gp; gp = gp->Next()) {
          const char *tmp1, *tmp2;
          tmp1 = gp->GetChan()->getName();
          tmp2 = inst->GetChan()->BaseType()->getName();
          if (strcmp(tmp1,tmp2) == 0 && gp->GetDir() == inst->GetDir()) {
            inst->SetSM(gp);
          }
        }
      }
    }
  }
  return;
}

bool create_inst_port(act_boolean_netlist_t *bnl, 
                       act_connection *c, 
                       int input, 
                       StateMachine *sm, 
                       StateMachineInst *smi, int func) {

  Scope *cs = bnl->p->CurScope();

  Port *p;
  ActId *tmp_id;
  ValueIdx *tmp_v;
  act_connection *tmp_c;
  unsigned int tmp_d = 0;
  unsigned int tmp_w = 0;
  unsigned int chan = 0;
  ihash_bucket *hb;

  tmp_id = c->toid();
  tmp_v = tmp_id->rootVx(cs);
  tmp_c = c;
  tmp_d = input;

  hb = ihash_lookup(bnl->cH, (long)tmp_c);
  act_booleanized_var_t *bv;
  
  if (hb) {
    bv = (act_booleanized_var_t *)hb->v;

    if (func == 0) {
      if (bv->used == 0 && bv->usedchp == 0) { return false; }
      InstType *it = bv->id->getvx()->t;
  
      if (TypeFactory::isChanType(it->BaseType())) {
        if (TypeFactory::isUserType(it)) {
          tmp_w = bv->width;
          chan = bv->ischan;
          p = new Port(tmp_d, tmp_w, chan, 0, tmp_v, tmp_id, tmp_c);
          p->SetOwner(smi);
          p->SetInst();
          smi->AddPort(p);
          sm->AddInstPortPair(c,p);
        } else {
          Chan *tmp_chan = dynamic_cast<Chan*>(it->BaseType());
          auto it1 = tmp_c->getvx()->t;
          // chan(user type)
          if (TypeFactory::isUserType(tmp_chan->datatype())) {
            auto it2 = tmp_chan->datatype()->BaseType();
            UserDef *ud_tmp = dynamic_cast<UserDef *>(it2);
            std::map<InstType *,std::vector<std::pair<ActId *,int>>> port_id_tmp;
            get_full_id(bnl,tmp_chan->datatype(),tmp_id,port_id_tmp);
            p = new Port(tmp_d, 0, 1, 0, tmp_v, tmp_id, tmp_c);
            p->SetOwner(smi);
            p->SetInst();
            smi->AddPort(p);
            sm->AddInstPortPair(c,p);
            for (auto id : port_id_tmp) {
              for (auto pp : id.second) {
                tmp_id = pp.first;
                tmp_v = pp.first->rootVx(cs);
                tmp_w = pp.second;
                p = new Port(tmp_d, tmp_w, 0, 0, tmp_v, tmp_id, tmp_c);
                p->SetType(1);
                p->SetOwner(smi);
                p->SetInst();
                smi->AddPort(p);
                sm->AddInstPortPair(c,p);
              }
            }
          // chan(base type)
          } else {
            tmp_w = bv->width;
            chan = bv->ischan;
            p = new Port(tmp_d, tmp_w, chan, 0, tmp_v, tmp_id, tmp_c);
            p->SetOwner(smi);
            p->SetInst();
            smi->AddPort(p);
            sm->AddInstPortPair(c,p);
          }
        }
      } else {
        tmp_w = bv->width;
        chan = bv->ischan;
        p = new Port(tmp_d, tmp_w, chan, 0, tmp_v, tmp_id, tmp_c);
        p->SetOwner(smi);
        p->SetInst();
        smi->AddPort(p);
        sm->AddInstPortPair(c,p);
      }
    } else {
      if (bv->used == 0) { return false; }
      tmp_w = 1;
      chan = 0;
      p = new Port(tmp_d,tmp_w, chan, 0, tmp_v, tmp_id, tmp_c);
      p->SetOwner(smi);
      p->SetInst();
      smi->AddPort(p);
      sm->AddInstPortPair(c,p);
    }
  }
 
  return true;
}

//Adding all instances in the current process
void add_instances(CHPProject *cp, act_boolean_netlist_t *bnl, StateMachine *sm) {

  Scope *cs = bnl->p->CurScope();

  ActUniqProcInstiter i(cs);
  
  StateMachineInst *smi;

  bool ip = true;
  int icport = 0;
  int ipport = 0;

  Port *tp = NULL;
  std::map<act_connection*, std::set<StateMachineInst*> > owners;

  InstType *it;

  ActId *inst_id = NULL;
  for (i = i.begin(); i != i.end(); i++) {
    ValueIdx *vx = *i;
    Process *p = dynamic_cast<Process *>(vx->t->BaseType());
    if (BOOL->getBNL(p)->isempty) {
      continue;
    }

    inst_id = new ActId(vx->getName());

    act_languages *lang = p->getlang();  
    act_boolean_netlist_t *sub;
    sub = BOOL->getBNL (p);

    int lev = get_proc_level(p,inst_id);

    int ports_exist = 0;
    for (int j = 0; j < A_LEN(sub->chpports); j++) {
      if (sub->chpports[j].omit == 0) {
        ports_exist = 1;
        break;
      }
    }
    if (ports_exist == 1) {
      if (vx->t->arrayInfo()) {
        Arraystep *as = vx->t->arrayInfo()->stepper();
        while (!as->isend()) {
          if (vx->isPrimary (as->index())) {
            char *ar = as->string();
            smi = new StateMachineInst(p,vx,ar);
            if (lev == ACT_MODEL_CHP && lang->getchp()) {
              for (auto j = 0; j < A_LEN(sub->chpports); j++){
                if (sub->chpports[j].omit == 1) { continue; }
                if (sub->chpports[j].c->toid()->isFragmented(sub->cur)) { 
                  icport++;
                  continue; 
                }
                act_connection *c = bnl->instchpports[icport]->toid()->Canonical(cs);
                ip = create_inst_port(bnl,c,sub->chpports[j].input,sm,smi,0);
                icport++;
                if (ip) {
                  owners[c].insert(smi);
                }
              }
              for (auto j = 0; j < A_LEN(sub->ports); j++) {
                if (sub->ports[j].omit == 1) { continue; }
                ipport++;
              }
            } else if (lev == ACT_MODEL_PRS && lang->getprs()) {
              smi->SetPrs();
              for (auto j = 0; j < A_LEN(sub->chpports); j++) {
                if (sub->chpports[j].omit == 1) { continue; }
                icport++;
              }
              for (auto j = 0; j < A_LEN(sub->ports); j++) {
                if (sub->ports[j].omit == 1) { continue; }
                act_connection *c = bnl->instports[ipport]->toid()->Canonical(cs);
                act_connection *tmp = bnl->instports[ipport];
                ip = create_inst_port(bnl,c,sub->ports[j].input,sm,smi,1);
                ipport++;
                if (ip) {
                  while (c->parent) {
                    owners[c].insert(smi);
                    c = c->parent;
                  }
                }
              }
            } else {
              if (Act::lang_subst) {
                for (auto j = 0; j < A_LEN(sub->chpports); j++){
                  if (sub->chpports[j].omit == 1) { continue; }
                  if (sub->chpports[j].c->toid()->isFragmented(sub->cur)) { 
                    icport++;
                    continue; 
                  }
                  act_connection *c = bnl->instchpports[icport]->toid()->Canonical(cs);
                  ip = create_inst_port(bnl,c,sub->chpports[j].input,sm,smi,0);
                  icport++;
                  if (ip) {
                    owners[c].insert(smi);
                  }
                }
                for (auto j = 0; j < A_LEN(sub->ports); j++) {
                  if (sub->ports[j].omit == 1) { continue; }
                  ipport++;
                }
              } else {
                printf("NO WAY\n");
              }
            }
            sm->AddInst(smi);
          }
          as->step();
        }
      } else {
        char *ar = NULL;
        smi = new StateMachineInst(p,vx,ar);
        if (lev == ACT_MODEL_CHP && lang->getchp()) {
          for (auto j = 0; j < A_LEN(sub->chpports); j++){
            if (sub->chpports[j].omit == 1) { continue; }
            if (sub->chpports[j].c->toid()->isFragmented(sub->cur)) { 
              icport++;
              continue; 
            }
            act_connection *c = bnl->instchpports[icport]->toid()->Canonical(cs);
            ip = create_inst_port(bnl,c,sub->chpports[j].input,sm,smi,0);
            icport++;
            if (ip) {
              owners[c].insert(smi);
            }
          }
          for (auto j = 0; j < A_LEN(sub->ports); j++) {
            if (sub->ports[j].omit == 1) { continue; }
            ipport++;
          }
        } else if (lev == ACT_MODEL_PRS && lang->getprs()) {
          smi->SetPrs();
          for (auto j = 0; j < A_LEN(sub->chpports); j++) {
            if (sub->chpports[j].omit == 1) { continue; }
            icport++;
          }
          for (auto j = 0; j < A_LEN(sub->ports); j++) {
            if (sub->ports[j].omit == 1) { continue; }
            act_connection *c = bnl->instports[ipport]->toid()->Canonical(cs);
            act_connection *tmp = bnl->instports[ipport];
            ip = create_inst_port(bnl,c,sub->ports[j].input,sm,smi,1);
            ipport++;
            if (ip) {
              while (c->parent) {
                owners[c].insert(smi);
                c = c->parent;
              }
            }
          }
        } else {
          if (Act::lang_subst) {
            for (auto j = 0; j < A_LEN(sub->chpports); j++){
              if (sub->chpports[j].omit == 1) { continue; }
              if (sub->chpports[j].c->toid()->isFragmented(sub->cur)) { 
                icport++;
                continue; 
              }
              act_connection *c = bnl->instchpports[icport]->toid()->Canonical(cs);
              ip = create_inst_port(bnl,c,sub->chpports[j].input,sm,smi,0);
              icport++;
              if (ip) {
                owners[c].insert(smi);
              }
            }
            for (auto j = 0; j < A_LEN(sub->ports); j++) {
              if (sub->ports[j].omit == 1) { continue; }
              ipport++;
            }
          } else {
            printf("NO WAY\n");
          }
        }
        sm->AddInst(smi);
      }
    }
  }

  for (auto pp : owners) {
    int src_id = 0;
    int dst_id = 0;

    if (pp.second.size() < 2) { continue; }
    act_connection *oc = pp.first;
    auto tmp1 = pp.second.begin();
    auto tmp2 = next(tmp1,1);
    if ((*tmp1)->GetPrs() == (*tmp2)->GetPrs()) { continue; }
    src_id = (*tmp1)->FindPort(pp.first);
    dst_id = (*tmp2)->FindPort(pp.first);
    Port *tp;
    if (src_id == -1) { tp = (*tmp2)->GetPorts()[dst_id]; } 
    if (dst_id == -1) { tp = (*tmp1)->GetPorts()[src_id]; }
    sm->AddInstPortPair(oc,tp);
    if (tp->GetDir() == 0) {
      src_id = 0;
      dst_id = 1;
    } else {
      src_id = 1;
      dst_id = 0;
    }
    StateMachineInst *gsmi = new StateMachineInst();
    gsmi->SetGlue();
    gsmi->SetSRC((*tmp1)->GetVx());
    gsmi->SetDST((*tmp2)->GetVx());

    gsmi->SetGlueDir(src_id);
 
    it = bnl->cur->FullLookup (pp.first->toid(), NULL);
    if (TypeFactory::isUserType(it->BaseType()) && 
        TypeFactory::isChanType(it->BaseType())) {
      gsmi->SetChan(it);
    } else {
      continue;
    }
 
    Port *gp;
    gp = new Port(tp);
    gp->SetOwner(gsmi);
    gp->FlipDir();
    gsmi->AddPort(gp);
    
    sm->AddInst(gsmi);
  }

  return;
}

void declare_vars (Scope *cs, act_boolean_netlist_t *bnl, StateMachine *tsm)
{

  Variable *var = NULL;

  int type = 0;
  int chan = 0;
  int port = 0;
  int dyn = 0;
  ValueIdx *vx = NULL;
  act_connection *con = NULL;

  int is_port = 0;
  int is_i_port = 0;
  int io = 0;

  phash_bucket_t *hb;
  phash_iter_t hi;

  //Static can be reg or wire
  phash_iter_init(bnl->cH, &hi);
  while ((hb = phash_iter_next(bnl->cH, &hi))) {
    act_booleanized_var_t *bv = (act_booleanized_var_t *)hb->v;
    is_port = bv->ischpport;
    con = bv->id->toid()->Canonical(cs);
    vx = con->getvx();
    ///
    //Channel Type
    if (bv->isint == 0 && bv->ischan == 1) {
      if (bv->ischpport == 0) {
        //Instance interconnection
        std::set<StateMachineInst *> owners;
        std::map<act_connection *, std::vector<Port *>> ports = tsm->GetInstPorts();
        for (auto port : ports[con]) {
          if (port->GetSMI()) owners.insert(port->GetSMI());
        }
        int num_of_owners = owners.size();
        if (num_of_owners == 1 && ports[con][0]->GetDir() == 0) { type = 1; }
        else if (num_of_owners == 1 && ports[con][0]->GetDir() == 1) { type = 0; }
        else if (num_of_owners > 1) { type = 2; }
        else { type = 3; }
      } else {
        type = 1;
      }
    }
    //Integer Type
    else if (bv->isint == 1 && bv->ischan == 0) {
      if (bv->ischpport == 0) {
        if (tsm->GetInstPorts()[con].size() >= 1) { type = 1; }
        else { type = 0; }
      }
    }
    else if (bv->isint == 0 & bv->ischan == 0) {
      type = 1;
    }
    chan = bv->ischan;
    if (bv->isglobal == 1) {
      port = 1;
    } else {
      port = bv->ischpport;
    }
    dyn = 0;

    InstType *it = vx->t;
    if (TypeFactory::isUserType(it->BaseType())) {
      UserDef *ud_tmp = dynamic_cast<UserDef *> (it->BaseType());
      if (chan == 1) {
      // FIXME: This need to be tested
        std::map<InstType *,std::vector<std::pair<ActId *,int>>> var_id_tmp;
        get_full_id(bnl,it,con->toid(),var_id_tmp);
        var = new Variable(type, chan, port, dyn, con, con->toid());
        var->AddDimension(0);
        tsm->AddVar(var);
        for (auto inst : var_id_tmp) {
          for (auto var_id : inst.second) {
            if (type == 3) { type = 0; }
            var = new Variable(type, 0, port, dyn, con, var_id.first);
            var->AddDimension(var_id.second);
            tsm->AddVar(var);
          }
        }
      } else {
        var = new Variable(type, chan, port, dyn, con, con->toid());
        var->AddDimension(bv->width-1);
        tsm->AddVar(var);
      }
    } else {
      if (TypeFactory::isChanType(it->BaseType())) {
        Chan *ch_tmp = dynamic_cast<Chan *> (it->BaseType());
        it = ch_tmp->datatype();
        if (TypeFactory::isUserType(it->BaseType())) {
          std::map<InstType *,std::vector<std::pair<ActId *,int>>> var_id_tmp;
          get_full_id(bnl,it,con->toid(),var_id_tmp);
          for (auto inst : var_id_tmp) {
            for (auto var_id : inst.second) {
              if (type == 3) { 
                var = new Variable(0, 0, port, dyn, con, var_id.first);
              } else {
                var = new Variable(type, 0, port, dyn, con, var_id.first);
              }
              var->AddDimension(var_id.second-1);
              tsm->AddVar(var);
            }
          }
          var = new Variable(type, chan, port, dyn, con, con->toid());
          var->AddDimension(0);
          tsm->AddVar(var);
        } else {
          var = new Variable(type, chan, port, dyn, con, con->toid());
          var->AddDimension(bv->width-1);
          tsm->AddVar(var);
        }
      } else {
        var = new Variable(type, chan, port, dyn, con, con->toid());
        var->AddDimension(bv->width-1);
        tsm->AddVar(var);
      }
    }
  }

  //Dynamic is always register
  phash_iter_init(bnl->cdH, &hi);
  while ((hb = phash_iter_next(bnl->cdH, &hi))) {
    act_dynamic_var_t *dv = (act_dynamic_var_t *)hb->v;
    con = dv->id;
    vx = con->toid()->rootVx(cs);
  
    type = 0;
    chan = 0;
    port = 0;
    dyn = 1;
  
    var = new Variable(type, chan, port, dyn, con, con->toid());
  
    var->AddDimension(dv->width-1);
    for (auto i = 0; i < dv->a->nDims(); i++) {
      var->AddDimension(dv->a->range_size(i));
    }
    tsm->AddVar(var);
  }

  return;

}

//Fuction to find and add empty Data to only
//initialize variables at reset.
//Example: *[[#X]]
void init_unused_ports (StateMachine *tsm)
{

  int used = 0;

  for (auto p : tsm->GetPorts()) {

    //Only check HSData because we interested in channel used
    //just for probing
    for (auto c : tsm->GetHSData()) {
      if (c.first == p->GetCon()) {
        used = 1;
        break;
      }
    }
    if (used == 0) {
      for (auto ip : tsm->GetInstPorts()) {
        if (ip.first == p->GetCon()) {
          used = 1;
          break;
        }
      }
    }
    if (used == 0 && p->GetChan()) {
      CHPData *d = new CHPData(4, tsm->GetProc(), tsm, NULL, NULL, p->GetCon()->toid(), p->GetCon()->toid());
      tsm->AddHS(p->GetCon(), d);
    }
    used = 0;
  }

  return;
}

void visit_user_t(act_boolean_netlist_t *bnl, UserDef *ud, ActId *id,
                    std::map<InstType *,
                            std::vector<
                              std::pair<ActId *,int>>> &ports) {

  ActId *tl = id;
  while (tl->Rest()) {
    tl = tl->Rest();
  }

  for (auto i = 0; i < ud->getNumPorts(); i++) {
    ActId *port_id = new ActId(ud->getPortName(i));
    InstType *pit = ud->getPortType(i);
    tl->Append(port_id);
    get_full_id(bnl,pit,id,ports);
    tl->prune();
    delete port_id;
  }
  return;
};

void get_full_id (act_boolean_netlist_t *bnl, InstType *it, ActId *id,
                        std::map<InstType *, 
                                std::vector<
                                  std::pair<ActId *, int>>> &ports) {

  std::pair<ActId*, int> tmp;

  ActId *tl = id;
  while (tl->Rest()) {
    tl = tl->Rest();
  }

  if (TypeFactory::isBoolType(it) || 
      TypeFactory::isIntType(it)) {
    if (it->arrayInfo()) {
      Arraystep *as = it->arrayInfo()->stepper();
      while (!as->isend()) {
        Array *a = as->toArray();
        tl->setArray(a);
        tmp.first = id->Clone();
        tmp.second = TypeFactory::bitWidth(it);
        ports[it].push_back(tmp);
        tl->setArray(NULL);
        delete a;
        as->step();
      }
      delete as;
    } else {
      tmp.first = id->Clone();
      tmp.second = TypeFactory::bitWidth(it);
      ports[it].push_back(tmp);
    }
  }
  if (TypeFactory::isUserType(it)) {
    UserDef *ud = dynamic_cast<UserDef *>(it->BaseType());
    if (it->arrayInfo()) {
      Arraystep *as = it->arrayInfo()->stepper();
      while (!as->isend()) {
        Array *a = as->toArray();
        tl->setArray(a);
        visit_user_t(bnl, ud, id, ports);
        tl->setArray(NULL);
        delete a;
        as->step();
      }
      delete as;
    } else {
      visit_user_t(bnl, ud, id, ports);
    }
  }

  return;
}

void extract_vars (Channel *ch, act_chp_lang_t *e, std::vector<act_connection *> &vars) {

  if (e->type == ACT_CHP_ASSIGN) {
    act_connection *id = e->u.assign.id->Canonical(ch->CurScope());
    if (std::find(vars.begin(), vars.end(), id) == vars.end()) { 
      vars.push_back(id);
    }
  } else if (e->type == ACT_CHP_COMMA | e->type == ACT_CHP_SEMI) {
    list_t *l = NULL;
    listitem_t *li = NULL;
    act_chp_lang_t *cl = NULL;
    l = e->u.semi_comma.cmd;
    for (li = list_first(l); li; li = list_next(li)) {
      cl = (act_chp_lang_t *)list_value(li);
      extract_vars(ch, cl, vars);
    }
  } else if (e->type == ACT_CHP_SELECT | e->type == ACT_CHP_SELECT_NONDET |
              e->type == ACT_CHP_LOOP | e->type == ACT_CHP_DOLOOP) {
    for (auto gg = e->u.gc; gg; gg = gg->next) {
      if (gg->s) { extract_vars(ch, gg->s, vars); }
    }
  }

  return;
}

act_chp_lang_t *create_chp_comm (Channel *ch, int dir) {
  act_chp_lang_t *cl = new act_chp_lang_t;

  ActId *ch_id = new ActId("chan");
  ActId *var_id = NULL;
  if (dir == 0) {
    var_id = new ActId("send_data");
    cl->type = ACT_CHP_RECV;
    cl->u.comm.chan = ch_id;
    cl->u.comm.var = var_id;
  } else {
    var_id = new ActId("recv_data");
    cl->type = ACT_CHP_SEND;
    cl->u.comm.chan = ch_id;
    cl->u.comm.var = var_id;
  }
  cl->u.comm.e = NULL;
  cl->u.comm.flavor = 0;
  cl->u.comm.convert = 0;

  return cl;
}

act_chp_lang_t *create_chp_loop (act_chp_lang_t *semi) {

  act_chp_gc *gc = new act_chp_gc;
  gc->id = NULL;
  gc->lo = NULL; gc->hi = NULL;
  gc->g = NULL;
  gc->s = semi;
  gc->next = NULL;

  act_chp_lang_t *cl = new act_chp_lang_t;
  cl->type = ACT_CHP_LOOP;
  cl->u.gc = gc;
  cl->label = NULL;
  cl->space = NULL;

  return cl;
}

act_chp_lang_t *create_chp_semi (list_t *l) {

  act_chp_lang_t *cl = new act_chp_lang_t;
  cl->type = ACT_CHP_SEMI;
  cl->u.semi_comma.cmd = l;
  cl->label = NULL;
  cl->space = NULL;

  return cl;
}

Expr *create_channel_probe_expr (Channel *ch) {
  //l-channel id; r - NULL
  Expr *pe = new Expr;
  pe->type = E_PROBE;
  ActId *ch_id = new ActId("self");
  pe->u.e.l = (expr*)ch_id;

  return pe;
}

act_chp_lang_t *create_chp_wait_probe (Channel *ch) {

  Expr *pe = create_channel_probe_expr(ch);
  act_chp_gc_t *gc = new act_chp_gc_t;
  gc->id = NULL;
  gc->lo = NULL;
  gc->hi = NULL;
  gc->g = pe;
  gc->s = NULL;
  gc->next = NULL;

  act_chp_lang_t *cl = new act_chp_lang_t;
  cl->type = ACT_CHP_SELECT;
  cl->label = NULL;
  cl->space = NULL;
  cl->u.gc = gc;

  return cl;
}

//0 send, 1 recv
//construct aritificial CHP of the following shape;
//SEND:  INIT; *[[#CHP_RECV]; SET; UP; REST; CHP_RECV ]
//RECV:  INIT; *[GET; CHP_SEND; UP; REST ]
act_chp_lang_t *create_channel_chp (Channel *ch, int func) {

  act_chp_lang_t *chan_chp = new act_chp_lang_t;

  list_t *l0 = list_new();
  if (func == 0) {
    list_append(l0,create_chp_wait_probe(ch));
    list_append(l0,ch->getMethod(ACT_METHOD_SET));
    list_append(l0,ch->getMethod(ACT_METHOD_SEND_UP));
    list_append(l0,ch->getMethod(ACT_METHOD_SEND_REST));
    list_append(l0,create_chp_comm (ch, 0));
  } else {
    list_append(l0,ch->getMethod(ACT_METHOD_GET));
    list_append(l0,create_chp_comm (ch, 1));
    list_append(l0,ch->getMethod(ACT_METHOD_RECV_UP));
    list_append(l0,ch->getMethod(ACT_METHOD_RECV_REST));
  }
  act_chp_lang_t *semi0 = create_chp_semi(l0);
  act_chp_lang_t *loop = create_chp_loop(semi0);
  
  list_t *l1 = list_new();
  if (func == 0) {
    list_append(l1,ch->getMethod(ACT_METHOD_SEND_INIT));
  } else {
    list_append(l1,ch->getMethod(ACT_METHOD_RECV_INIT));
  }
  list_append(l1, loop);
  chan_chp = create_chp_semi(l1);

  return chan_chp;
}

void create_glue_machine (CHPProject *cp, 
                          act_boolean_netlist_t *bnl, InstType *it) {

  Channel *ch = dynamic_cast<Channel *> (it->BaseType());
  std::map<InstType *,std::vector<std::pair<ActId *, int>>> ports;
  for (auto i = 0; i < ch->getNumPorts(); i++) {
    ActId *port_id = new ActId(ch->getPortName(i));
    InstType *pit = ch->getPortType(i);
    get_full_id(bnl,pit,port_id,ports);
  }

  for (auto i = 0; i < 2; i++) {
    StateMachine *sm = new StateMachine();
    act_chp_lang_t *chan_chp = create_channel_chp(ch,i);
    sm->SetChan(ch);
    sm->SetNumber(0);
    sm->SetDir(i);
    Variable *var = new Variable(i+4,0,0,0,NULL,NULL);
    var->AddDimension(TypeFactory::bitWidth(it)-1);
    sm->AddVar(var);
    traverse_chp(NULL, ch, chan_chp, sm, sm, NULL, NULL, ACT_CHP_NULL, 0);
    Port *p = new Port(i == 0 ? 1 : 0,TypeFactory::bitWidth(ch),1,1);
    p->SetId(new ActId(ch->getName()));
    p->SetOwner(sm);
    sm->AddPort(p);
    for (auto inst : ports) {
      for (auto pp : inst.second) {
        cp->GetChan()[it->BaseType()].push_back(pp.first);
        int dir = ch->chanDir(pp.first,i) == 1 ? 1 : 0;
        Port *p = new Port(dir,1,0,1);
        p->SetId(pp.first);
        p->SetOwner(sm);
        sm->AddPort(p);
      }
    }
    cp->AppendGlue(sm);
    cp->GetGlue()[it->BaseType()] = sm;
  }

  return;
}

//Function to collect user defined channel types
void collect_chan (Process *p, CHPProject *cp) {

  Scope *cs = p->CurScope();

  ActUniqProcInstiter i(cs);

  for (i = i.begin(); i != i.end(); i++) {
    ValueIdx *vx = *i;
    collect_chan (dynamic_cast<Process *>(vx->t->BaseType()), cp);
  }

  act_boolean_netlist_t *bnl = BOOL->getBNL(p);

  int exist = 0;
  ActInstiter itr(cs);
  for (itr = itr.begin(); itr != itr.end(); itr++) {
    ValueIdx *vx = *itr;
    InstType *it = vx->t;
    if (TypeFactory::isChanType(it->BaseType())) {
      if (TypeFactory::isUserType(it->BaseType())) {
        for (auto itt : cp->GetChan()) {
          if (itt.first == it->BaseType()) { exist = 1; }
        }
        if (exist == 0) {
          create_glue_machine(cp,bnl,it);
        } else {
          exist = 0;
        }
      }
    }
  }

  return;
}

//Function to traverse act data strcutures and walk
//through entire act project hierarchy while building
//state machine for each process
void traverse_act (Process *p, CHPProject *cp, int opt, char *pn) {

  act_boolean_netlist_t *bnl = BOOL->getBNL(p);

  if (bnl->visited) {
    return;
  }
  bnl->visited = 1;

  if (bnl->isempty) {
    return;
  }

  act_languages *lang;
  act_chp *chp;
  act_prs *prs;
  Scope *cs;

  if (p) {
    lang = p->getlang();
    cs = p->CurScope();
  }

  if (lang) {
    chp = lang->getchp();
    prs = lang->getprs();
  }

  ActUniqProcInstiter i(cs);

  for (i = i.begin(); i != i.end(); i++) {
    ValueIdx *vx = *i;
    traverse_act (dynamic_cast<Process *>(vx->t->BaseType()), cp, opt, pn);
  }

  act_chp_lang_t *chp_lang = NULL;
  if (chp) {
    chp_lang = chp->c;
  }

  act_prs_lang_t *prs_lang = NULL;
  if (prs) {
    prs_lang = prs->p;
  }

  //Create state machine for the currect
  //process
  StateMachine *sm = new StateMachine();
  sm->SetProcess(p);
  sm->SetNumber(0);

  int lev = get_proc_level(p, NULL);

  //If PRS process add to the list and return
  if (lev == ACT_MODEL_CHP && chp_lang) {
    //add instances
    add_instances(cp, bnl, sm);
   
    //add ports
    add_ports(bnl, sm);

    declare_vars(cs, bnl, sm);
    //run chp traverse to build state machine
    if (lang->getchp()) {
      traverse_chp(p, NULL, chp_lang, sm, sm, NULL, NULL, ACT_CHP_NULL, opt);
    }
   
    //append linked list of chp project
    //processes
    cp->Append(sm);
  } else if (lev == ACT_MODEL_PRS && lang->getprs()) {
    //add ports
    add_ports(bnl, sm);
    sm->SetPrs(); 
    cp->Append(sm);
    return;
  } else {
    if  (Act::lang_subst) {
      //add instances
      add_instances(cp, bnl, sm);
     
      //add ports
      add_ports(bnl, sm);

      // declare vars
      declare_vars(cs, bnl, sm);
      //run chp traverse to build state machine
      if (chp_lang) {
        traverse_chp(p, NULL, chp_lang, sm, sm, NULL, NULL, ACT_CHP_NULL, opt);
      }
     
      //append linked list of chp project
      //processes
      cp->Append(sm);
    } else { 
      printf("SOMETHING IS WRONG\n"); 
    }
  }

  return;
}

CHPProject *build_machine (Process *p, int opt, char *pn) {

  CHPProject *cp = new CHPProject();

  collect_chan(p, cp);

  prs_list = fopen("prs_list.txt", "w");
  traverse_act (p, cp, opt, pn);
  for (auto sm = cp->Head(); sm; sm = sm->Next()) {
    init_unused_ports(sm);
  }
  fclose(prs_list);

  map_instances(cp);

  return cp;
}

}
