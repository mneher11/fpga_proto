#include <common/int.h>
#include <act/act.h>
#include <act/passes/booleanize.h>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <math.h>

#include "state_machine.h"

namespace fpga {

FILE *output_file = stdout;
FILE *func_file = stdout;
extern ActBooleanizePass *BOOL;

std::map<int,std::vector<int>> resize_func_tracker;

void get_module_name (Process *p, std::string &str) {

  const char *mn = p->getName();
  std::string tmp1 = "";
  std::string tmp2 = "";

  if (p->getns() && p->getns()->Parent()) {
    ActNamespace *an = p->getns();
    tmp1 = tmp2 + an->getName() + "_" + tmp1;
    an = an->Parent();
    while (an->Parent()) {
      tmp1 = tmp2 + an->getName() + "_" + tmp1;
      an = an->Parent();
    }
  }
  str += tmp1;

  int len = strlen(mn);

  for (auto i = 0; i < len; i++) {
    if (mn[i] == '<') {
      continue;
    } else if (mn[i] == '>') {
      continue;
    } else if (mn[i] == ',') {
      str += "_";
    } else if (mn[i] == '{') {
      str += "_";
    } else if (mn[i] == '}') {
      str += "_";
    } else {
      str += mn[i];
    }
  }

  return;
}

void get_chan_name (Channel *ch, std::string &str) {

  const char *cn = ch->getName();
  std::string tmp1 = "";
  std::string tmp2 = "";

  if (ch->getns() && ch->getns()->Parent()) {
    ActNamespace *an = ch->getns();
    tmp1 = tmp2 + an->getName() + "_" + tmp1;
    an = an->Parent();
    while (an->Parent()) {
      tmp1 = tmp2 + an->getName() + "_" + tmp1;
      an = an->Parent();
    }
  }
  str += tmp1;

  int len = strlen(cn);

  for (auto i = 0; i < len; i++) {
    if (cn[i] == 0x3c) {
      continue;
    } else if (cn[i] == 0x3e) {
      continue;
    } else if (cn[i] == 0x2c) {
      continue;
    } else {
      str += cn[i];
    }
  }

  return;
}

void print_array_ref (ActId *id, StateMachine *scope, std::string &str) {

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

bool NeedResizeFunc(int from, int to) {

  if (resize_func_tracker.find(from) != resize_func_tracker.end()) {
    if (std::find(resize_func_tracker[from].begin(),
                  resize_func_tracker[from].end(),to) != 
                    resize_func_tracker[from].end()) {
      return false;
    }
  }

  resize_func_tracker[from].push_back(to);

  return true;
}

void PrintResizeFunc(int from, int to) {
  std::string func;
  std::string s_from = std::to_string(from);
  std::string s_to = std::to_string(to);
  std::string func_name = "resize_" + s_from + "_to_" + s_to;
  func = "function [" + s_to + "-1:0] " + func_name + ";\n";
  func = func + "  input [" + s_from + "-1:0] in;\n  begin\n";
  if (from > to) {
    func = func + "    " + func_name + " = in[" + s_to + "-1:0];\n";
  } else if (to > from) {
    int delta = to - from;
    std::string s_zero = "{" + std::to_string(delta) + "{1'b0}}";
    func = func + "    " + func_name + " = {" + s_zero + ",in};\n";
  } else {
    func = func + "    " + func_name + " = in;\n";
  }
  func = func + "  end\n";
  func += "endfunction\n\n";
  fprintf(func_file,"%s",func.c_str());
}

int GetExprResWidth (Expr *e, StateMachine *scope) {

  unsigned long l = 0;
  unsigned long r = 0;

  switch (e->type) {
    case (E_AND):
    case (E_OR):
    case (E_XOR):
    case (E_PLUS):
    case (E_MINUS):
      return GetExprResWidth(e->u.e.l, scope);
      break;
    case (E_MULT):
      l = GetExprResWidth(e->u.e.l, scope);
      r = GetExprResWidth(e->u.e.r, scope);
      return l + r;
      break;
    case (E_DIV):
      return GetExprResWidth(e->u.e.l, scope);
      break;
    case (E_MOD):
    case (E_LSL):
    case (E_LSR):
    case (E_ASR):
      return GetExprResWidth(e->u.e.l, scope);
      break;
    case (E_NOT):
    case (E_UMINUS):
      break;
    case (E_LT): 
    case (E_GT): 
    case (E_LE): 
    case (E_GE): 
    case (E_EQ): 
    case (E_NE):
    case (E_TRUE):
    case (E_FALSE):
      return 1;
      break;
    case (E_QUERY):
      l = GetExprResWidth(e->u.e.r->u.e.l, scope);
      r = GetExprResWidth(e->u.e.r->u.e.r, scope);
      if (l > r) return l;
      else return r;
      break;
    case (E_LPAR): 
    case (E_RPAR):
    case (E_COLON):
    case (E_PROBE):
    case (E_COMMA):
    case (E_ANDLOOP):
    case (E_ORLOOP):
    case (E_RAWFREE):
    case (E_END):
    case (E_NUMBER):
    case (E_FUNCTION):
      return 0;
      break;
    case (E_INT): {
      BigInt *bi;
      if (e->u.ival.v_extra) {
        bi = (BigInt*)e->u.ival.v_extra;
        if (bi->getWidth() == 0) {
          return 0;
        } else {
          return bi->getWidth();
        }
      } else {
        unsigned long tmp1 = 0;
        unsigned long tmp2 = e->u.ival.v;
        while (tmp2) {
          tmp2 = tmp2 >> 1;
          tmp1 = tmp1 + 1;
        }
        if (tmp1 == 0) { tmp1 = 1; }
        return tmp1;
      }
      break;
    }
    case (E_VAR): {
      char tmp[1024];
      ActId *id;
      id = (ActId *)e->u.e.l;
      Scope *act_scope = NULL;
      act_scope = scope->GetProc()->CurScope();
      act_boolean_netlist_t *bnl;
      bnl = BOOL->getBNL(scope->GetProc());
      ihash_bucket *hb;
      act_booleanized_var_t *bv;
      hb = ihash_lookup(bnl->cH,(long)id->Canonical(act_scope));
      if (hb) {
        bv = (act_booleanized_var_t *)hb->v;
        return bv->width;
      } else {
        return 0;
      }
      break;
    }
    case (E_CONCAT): {
      Expr *tmp_expr = e;
      while (tmp_expr) {
        l += GetExprResWidth(tmp_expr->u.e.l, scope);
        if (tmp_expr->u.e.r) {
          tmp_expr = tmp_expr->u.e.r;
        } else { 
          tmp_expr = NULL; 
        }
      }
      return l;
      break;
    }
    case (E_BITFIELD): {
      unsigned int l;
      unsigned int r;
      l = e->u.e.r->u.e.r->u.ival.v;
      if (e->u.e.r->u.e.l) {
        r = e->u.e.r->u.e.l->u.ival.v;
      } else {
        r = l;
      }
      return l-r+1;
      break;
    }
    case (E_COMPLEMENT):
      return GetExprResWidth(e->u.e.l, scope);
      break;
    case (E_REAL):
      return 0;
      break;
    case (E_BUILTIN_INT): {
      if (e->u.e.r) {
        return e->u.e.r->u.ival.v;
      } else {
        return GetExprResWidth(e->u.e.l, scope);
      }
      break;
    }
    case (E_BUILTIN_BOOL):
      return 1;
      break;
    default:
      break;
  }

  return 0;
}

void PrintExpression(Expr *e, StateMachine *scope, std::string &str) {
  if (e->type == E_NOT || e->type == E_COMPLEMENT) { str += " ~("; } 
  else if (e->type == E_UMINUS) { str += " -("; } 
  else if (e->type == E_CONCAT || e->type == E_COMMA || e->type == E_VAR || e->type == E_INT) { str = str; }
  else { str += "("; }
  switch (e->type) {
    case (E_AND): {
      PrintExpression(e->u.e.l, scope, str); str += " & "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_OR): {
      PrintExpression(e->u.e.l, scope, str); str += " | "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_NOT): {
      PrintExpression(e->u.e.l, scope, str);
      break;
    }
    case (E_PLUS): {
      PrintExpression(e->u.e.l, scope, str); str += " + "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_MINUS): {
      PrintExpression(e->u.e.l, scope, str); str += " - "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_MULT): {
      PrintExpression(e->u.e.l, scope, str); str += " * "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_DIV): {
      PrintExpression(e->u.e.l, scope, str); str += " / "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_MOD): {
      PrintExpression(e->u.e.l, scope, str); str += " % "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_LSL): {
      PrintExpression(e->u.e.l, scope, str); str += " << "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_LSR): {
      PrintExpression(e->u.e.l, scope, str); str += " >> "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_ASR): {
      PrintExpression(e->u.e.l, scope, str); str += " >>> "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_UMINUS): {
      PrintExpression(e->u.e.l, scope, str);
      break;
    }
    case (E_XOR): {
      PrintExpression(e->u.e.l, scope, str); str += " ^ "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_LT): {
      PrintExpression(e->u.e.l, scope, str); str += " < "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_GT): {
      PrintExpression(e->u.e.l, scope, str); str += " > "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_LE): {
      PrintExpression(e->u.e.l, scope, str); str += " <= "; PrintExpression(e->u.e.r, scope, str);
      break;  
    }
    case (E_GE): {
      PrintExpression(e->u.e.l, scope, str); str += " >= "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_EQ): {
      PrintExpression(e->u.e.l, scope, str); str += " == "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_NE): {
      PrintExpression(e->u.e.l, scope, str); str += " != "; PrintExpression(e->u.e.r, scope, str);
      break;
    }
    case (E_INT): {
      BigInt *bi;
      if (e->u.ival.v_extra) {
        bi = (BigInt*)e->u.ival.v_extra;
        if (bi->getWidth() == 0) {
          str = str + "{0{1'b0}}";
        } else {
          str = str + std::to_string(bi->getWidth()) + "'d" + std::to_string(e->u.ival.v);
        }
      } else {
        str = str + "64'd" + std::to_string(e->u.ival.v);
      }
      break;
    }
    case (E_VAR): {
      char tmp[1024];
      ActId *id;
      id = (ActId *)e->u.e.l;
      Scope *act_scope = NULL;
      str += "\\";
      if (scope->GetProc()) {
        act_scope = scope->GetProc()->CurScope();
        act_boolean_netlist_t *bnl;
        bnl = BOOL->getBNL(scope->GetProc());
        act_dynamic_var_t *dv;
        dv = BOOL->isDynamicRef(bnl, id);
        if (!dv) {
          id->Canonical(act_scope)->toid()->sPrint(tmp,1024);
          str += tmp;
        } else {
          print_array_ref(id, scope, str);
        }
      } else {
        std::string self = "self";
        id->sPrint(tmp,1024);
        if (tmp == self) { 
          std::string ch = "";
          if (scope->GetChan()) get_chan_name(scope->GetChan(), ch);
          if (scope->GetDir() == 0) {
            str += "send_data";
          } else {
            str += "recv_data";
          }
        } else {
          str += tmp;
        }
      }
      break;
    }
    case (E_QUERY): {
      PrintExpression(e->u.e.l, scope, str); str += " ? ";
      PrintExpression(e->u.e.r->u.e.l, scope, str); str += " : \n\t\t"; PrintExpression(e->u.e.r->u.e.r, scope, str);
      break;
    }
    case (E_LPAR): {
      str += "LPAR\n";
      break;
    }
    case (E_RPAR): {
      str += "RPAR\n";
      break;
    }
    case (E_TRUE): {
      str += " 1'b1 ";
      break;  
    }
    case (E_FALSE): {
      str += " 1'b0 ";
      break;
    }
    case (E_COLON): {
      str += " : ";
      break;
    }
    case (E_PROBE): {
      char tmp[1024];
      ActId *id;
      Scope *act_scope;
      if (scope->GetProc()) {
        act_scope = scope->GetProc()->CurScope();
        id = (ActId *)e->u.e.l;
        act_connection *cc = id->Canonical(act_scope);
        StateMachine *sm = scope->GetPar();
        if (sm) {
          while (sm->GetPar()) { sm = sm->GetPar(); }
        }
        str += "\\";
	{
	  // need the canonical name
	  ActId *tid = cc->toid();
	  tid->sPrint (tmp, 1024);
	  delete tid;
	}
        str += tmp;
        if (sm) {
          if (sm->IsPort(cc) == 1) {
            str +="_ready";
          } else if (sm->IsPort(cc) == 2){
            str +="_valid";
          } else {
            str +="_valid";
          }
        } else {
          if (scope->IsPort(cc) == 1) {
            str +="_ready";
          } else if (scope->IsPort(cc) == 2){
            str +="_valid";
          } else {
            str +="_valid";
          }
        }
      } else {
        std::string ch = "";
        if (scope->GetChan()) get_chan_name(scope->GetChan(), ch);
        str += ch;
        str += "_valid";
      }
      break;
    }
    case (E_COMMA):
    case (E_CONCAT): {
      str += "{";
      Expr *tmp_expr = e;
      while (tmp_expr) {
        PrintExpression(tmp_expr->u.e.l, scope, str);
        if (tmp_expr->u.e.r) {
          str += " ,";
          tmp_expr = tmp_expr->u.e.r;
        } else { 
          tmp_expr = NULL; 
        }
      }
      str += " }";
      break;
    }
    case (E_BITFIELD): {
      if (e->u.e.l->type != E_VAR) {
	str += "( ( (";
      }
      PrintExpression (e->u.e.l, scope, str);
      unsigned int l;
      unsigned int r;
      l = e->u.e.r->u.e.r->u.ival.v;
      if (e->u.e.r->u.e.l) {
        r = e->u.e.r->u.e.l->u.ival.v;
      } else {
        r = l;
      }
      if (e->u.e.l->type != E_VAR) {
	str += ") ";
	if (r != 0) {
	  str += ">> " + std::to_string (r);
	}
	str += ") & " + std::to_string (l - r + 1) + "'b";
	for (int i=0; i < l - r + 1; i++) {
	  str += "1";
	}
	str += ")";
      }
      else {
	if (l!=r) {
	  str = str + " [" + std::to_string(l) + ":" + std::to_string(r) + "]";
	} else {
	  str = str + " [" + std::to_string(r) + "]";
	}
      }
      break;
    }
    case (E_COMPLEMENT): {
      PrintExpression(e->u.e.l, scope, str);
      break;
    }
    case (E_REAL): {
      str += std::to_string(e->u.ival.v);
      break;
    }
    case (E_ANDLOOP): {
      str += "ANDLOOP you mean?\n";
      break;
    }
    case (E_ORLOOP): {
      str += "ORLOOP you mean?\n";
      break;
    }
    case (E_BUILTIN_INT): {
      int tmp1, tmp2;
      if (e->u.e.r) {
        if (e->u.e.l->type == E_INT || e->u.e.l->type == E_VAR) {
          str.pop_back();
          PrintExpression(e->u.e.l, scope, str);
          return;
        } else {
          tmp1 = GetExprResWidth(e->u.e.l, scope);
          tmp2 = e->u.e.r->u.ival.v;
          if (tmp1 != tmp2) {
            if (NeedResizeFunc(tmp1,tmp2)) {
              PrintResizeFunc(tmp1,tmp2);
            }
            str += "resize_";
            str += std::to_string(tmp1);
            str += "_to_";
            str += std::to_string(tmp2);
            str += "(";
            PrintExpression(e->u.e.l, scope, str);
            str += " )";
          } else {
            PrintExpression(e->u.e.l, scope, str);
          }
        }
      } else {
        PrintExpression(e->u.e.l, scope, str);
      }
      break;
    }
    case (E_BUILTIN_BOOL): {
      PrintExpression(e->u.e.l, scope, str);
      break;
    }
    case (E_RAWFREE): {
      str += "RAWFREE\n";
      break;
    }
    case (E_END): {
      str += "END\n";
      break;
    }
    case (E_NUMBER): {
      str += "NUMBER\n";
      break;
    }
    case (E_FUNCTION): {
      str += "FUNCTION\n";
      break;
    }
    default: {
      str += "Whaaat?! "; str += std::to_string(e->type); str += "\n";
      break;
    }
  }
  if (e->type == E_CONCAT || e->type == E_COMMA || e->type == E_VAR || e->type == E_INT) { str = str; }
  else { str += " )"; }

  return;
}

void Condition::PrintScopeVar(StateMachine *sc, std::string &name){

  name = name + "sm" + std::to_string(sc->GetNum()) + "_";
  if (sc->GetPar()) { PrintScopeVar(sc->GetPar(), name); }

  return;
}

void Condition::PrintScopeParam(StateMachine *sc, std::string &name) {

  name = name + "SM" + std::to_string(sc->GetNum()) + "_";
  if (sc->GetPar()) { PrintScopeParam(sc->GetPar(), name); }

  return;
}

void Condition::PrintExpr(Expr *e, std::string &str) {
  PrintExpression(e, scope, str);
  return;
}

void Condition::PrintVerilogExpr(std::string &name) {

  StateMachine *sm;
  char tmp[1024];

  name += "assign ";
  PrintScopeVar(scope, name);
  switch (type) {
    case (0) :
      if (scope->GetChan()) {
        std::string ch = "";
        get_chan_name(scope->GetChan(), ch);
        name = name + "commu_compl_" + std::to_string(num) + " = \\" + ch + "_valid & \\" + ch + "_ready ";
      } else {
        u.v->sPrint(tmp,1024);
        name = name + "commu_compl_" + std::to_string(num) + " = \\" + tmp + "_valid & \\" + tmp + "_ready ";
      }
      break;
    case (1) :
      name = name + "guard_" + std::to_string(num) + " = ";
      PrintExpr(u.e, name);
      name += " ? 1'b1 : 1'b0 ";
      break;
    case (2) :
      sm = u.s->GetPar();
      name = name + "state_cond_" + std::to_string(num) + " = ";
      PrintScopeVar(sm,name);
      name = name + "state == "; 
      PrintScopeParam(sm,name);
      name = name + "STATE_" + std::to_string(u.s->GetNum());
      break;
    case (3) :
      name = name + "cond_" + std::to_string(num) + " = ";
      for (auto cc : u.c->c) {
        if (u.c->type == 2) { name += "!"; }
        PrintScopeVar(cc->GetScope(),name);
        if (cc->GetType() == 0) {
          name = name + "commu_compl_" + std::to_string(cc->GetNum());
        } else if (cc->GetType() == 1 || cc->GetType() == 4) {
          name = name + "guard_" + std::to_string(cc->GetNum());
        } else if (cc->GetType() == 2) {
          State *ss = cc->GetState();
          sm = ss->GetPar();
          name = name + "state_cond_" + std::to_string(cc->GetNum());
        } else {
          name = name + "cond_" + std::to_string(cc->GetNum());
        }
        if (cc != u.c->c[u.c->c.size()-1]) {
          if (u.c->type != 1) {
            name += " & ";
          } else {
            name += " | ";
          }
        }
      }
      break;
    case (4) :
      name = name + "excl_guard_" + std::to_string(num) + " = ";
      PrintExpr(u.e, name);
      name += " ? 1'b1 : 1'b0 ";
      break;
    default :
      fatal_error("!!!\n");
  }
  name += ";\n";

  return;
}

void Condition::PrintVerilogDecl(std::string &name) {

  name += "wire ";
  PrintScopeVar(scope, name);
  switch (type) {
  case (0) :
    name += "commu_compl_";
    break;
  case (1) :
    name +=  "guard_";
    break;
  case (2) :
    name +=  "state_cond_";
    break;
  case (3) :
    name +=  "cond_";
    break;
  case (4) :
    name +=  "excl_guard_";
   name += std::to_string(num);
   name += ";\n";
   name += "wire ";
   PrintScopeVar(scope, name);
   name +=  "guard_";
    break;
  default :
    fatal_error("!!!\n");
  }
  name += std::to_string(num);
  name += ";\n";

  return;
}

void Condition::PrintVerilogDeclRaw(std::string &name) {

  PrintScopeVar(scope, name);
  switch (type) {
  case (0) :
    name += "commu_compl_";
    break;
  case (1) :
    name +=  "guard_";
    break;
  case (2) :
    name +=  "state_cond_";
    break;
  case (3) :
    name +=  "cond_";
    break;
  case (4) :
    name +=  "excl_guard_";
    break;
  default :
    fatal_error("!!!\n");
  }
  name += std::to_string(num);

  return;
}

void Variable::PrintVerilog() {

  char tmp[1024];
  if (type < 4) 
   id->sPrint(tmp, 1024);

  std::string var;

  if (type == 0 || type == 3 || type == 4 || type == 5) { var = "reg\t"; } 
  else { var = "wire\t"; }

  if (dim[0] >= 1) { var = var + "[" + std::to_string(dim[0]) + ":0]"; }

  if (type == 4) {
    var = var + "\t\\send_data = 0";
  }else if (type == 5) {
    var = var + "\t\\recv_data = 0";
  } else {
    var = var + "\t\\" + tmp;
  }

  for (auto i = 1; i < dim.size(); i++) {
    if (isdyn == 1) { var += " "; }
    var = var + "[" + std::to_string(dim[i]-1) + ":0]";
  }
  var += " ;\n";

  if (ischan != 0) {
    if (type == 0 || type == 3) { var += "reg\t\\"; } 
    else { var += "wire\t\\"; }
    var = var + tmp + "_valid ;\n";

    if (type == 0 || type == 2) { var += "wire\t\\"; } 
    else { var += "reg\t\\"; }
    var = var + tmp + "_ready ;\n";
  }

  std::string init;
  if (isdyn == 1) {
    std::string n;
    init += "\n";
    for (auto i = 1; i < dim.size(); i++) { n = std::to_string(i); init = init + "integer \\" + tmp + n + " ;\n"; }
    init += "initial begin\n";
    for (auto i = 1; i < dim.size(); i++) { n = std::to_string(i); init = init+"\tfor (\\"+tmp+n+" =0;\\"+tmp+n+" <"+std::to_string(dim[i])+";\\"+tmp+n+" =\\"+tmp+n+" +1) begin\n"; }
    init = init + "\t\t\\" + tmp + " ";
    for (auto i = 1; i < dim.size(); i++) { init = init+"[\\" + tmp + std::to_string(i) + " ]"; }
    init += " <= 0;";
    for (auto i = 1; i < dim.size(); i++) { init += "\t\nend\n"; }
    init += "end\n\n";
  }

// FIXME: Do I need this?
 // if (con) {
 //   char tmp1[1024];
 //   act_connection *ct = con;
 //   while (ct->next != con) {
 //     ct = ct->next;
 //     ct->toid()->sPrint(tmp1,1024);
 //     var = var + "wire\t\\" + tmp1 + " ;\n";
 //     var = var + "assign \\" + tmp1 + " = \\" + tmp + " ;\n\n";
 //   }
 // }

  fprintf(output_file, "%s", var.c_str());
  var.clear();
  if (isdyn == 1) {
    fprintf(output_file, "%s", init.c_str());
    init.clear();
  }

  return;
}

void StateMachine::PrintVerilogVars() {
  for (auto v : vars) {
    if (v->IsPort() == 0) {
      v->PrintVerilog();
    }
  }
  fprintf(output_file, "\n");

  return;
}

void StateMachine::PrintVerilogWires() {

  std::string wire;

  for (auto c : csm) {
    c->PrintVerilogWires();
  }

  for (auto cc : guard_condition) {
    cc->PrintVerilogDecl(wire);
  }

  for (auto cc : state_condition) {
    cc->PrintVerilogDecl(wire);
  }

  for (auto cc : commu_condition) {
    cc->PrintVerilogDecl(wire);
  }

  for (auto cc : comma_condition) {
    cc->PrintVerilogDecl(wire);
  }
  fprintf(output_file, "%s\n", wire.c_str());
  wire.clear();

  return;
}

void State::PrintScopeParam(StateMachine *p, std::string &name) {

  name = name + "SM" + std::to_string(p->GetNum()) + "_";
  if (p->GetPar()) {
    PrintScopeParam(p->GetPar(), name);
  }

  return;
}

void State::PrintScopeVar(StateMachine *p, std::string &name) {

  name = name + "sm" + std::to_string(p->GetNum()) + "_";
  if (p->GetPar()) {
    PrintScopeVar(p->GetPar(), name);
  }

  return;
}

void StateMachine::PrintVerilogParameters(){

  std::string name;

  if (top) {
    name = "reg [31:0] ";
    top->PrintScopeVar(this, name);
    name += "state = 0;\n";
    for (auto i = 0; i < size; i++) {
      name += "localparam ";
      top->PrintScopeParam(this, name);
      name = name + "STATE_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    name += "\n";
    fprintf(output_file, "%s", name.c_str());
    for (auto c : csm) {
      c->PrintVerilogParameters();
    }
  }

  return;
}

void Port::PrintVerilog() {

  std::string port;
  char tmp[1024];
  port_name->sPrint(tmp,1024);

  if (dir == 0) {
    if (ischan != 0 && type == 0) {
      if (reg) { port = port + "\t,output reg\t\\" + tmp; } 
      else { port = port + "\t,output\t\\" + tmp; }
      port = port + "_valid\n\t,input\t\t\\" + tmp + "_ready\n";
    }
    if (ischan != 2 && width > 0) {
      if (reg) { port = port + "\t,output reg\t"; } 
      else { port = port + "\t,output\t"; }
    }
  } else {
    if (ischan != 0 && type == 0)  {
      if (reg) { port = port + "\t,output reg\t\\" + tmp; } 
      else {port = port + "\t,output\t\\" + tmp; }
      port = port + "_ready\n\t,input\t\t\\" + tmp + "_valid\n";
    }
    if (ischan != 2 && width > 0) { port += "\t,input\t\t"; }
  }
  if (ischan != 2 && width > 0) { port = port + "[" + std::to_string(width-1) + ":0]\t\\" + tmp; }
  fprintf(output_file, "%s\n", port.c_str());
  port.clear();

  return;
}

void StateMachine::PrintVerilogHeader(int sv) {
  std::string header;
  if (p) {
    header += "`timescale 1ns/1ps\n\n";
    header += "module \\";
    get_module_name(p, header);
    header += " (\n";
    header += "\t input\t\\clock\n";
    header += "\t,input\t\\reset\n";
    fprintf(output_file, "%s", header.c_str());
    header.clear();
    for (auto pp : ports) { pp->PrintVerilog(); }
    header += ");\n\n";
  }
  fprintf(output_file, "%s", header.c_str());
  header.clear();

  PrintVerilogWires();
  for (auto s : ssm) {
    s->PrintVerilogWires();
  }
  PrintVerilogVars();
  for (auto s : ssm) {
    s->PrintVerilogVars();
  }
  PrintVerilogParameters();
  for (auto s : ssm) {
    s->PrintVerilogParameters();
  }

  return;
}

void StateMachine::PrintScopeParam(StateMachine *p, std::string &str) {
  str = str + "SM" + std::to_string(p->GetNum()) + "_";
  if (p->par) { PrintScopeParam(p->GetPar(), str); }

  return;
}

void StateMachine::PrintScopeVar(StateMachine *p, std::string &str) {
  str = str + "sm" + std::to_string(p->GetNum()) + "_";
  if (p->par) { PrintScopeVar(p->GetPar(), str); }

  return;
}

void StateMachine::PrintVerilogState(std::vector<std::pair<State *, Condition *>> s, std::string &str) {
  for (auto ss : s) {
    if (ss.first->isPrinted()) { continue; }
    ss.first->PrintVerilog(str);
  }
  for (auto ss : s) {
    if (ss.first->isPrinted()) { continue; }
    PrintVerilogState(ss.first->GetNextState(), str);
  }
  return;
}

void State::PrintVerilog(std::string &str) {
  if (printed) { return; }
  printed = 1;
  for (auto c : ns) {
    c.first->PrintVerilog(str);
  }
  for (auto c : ns) {
    str += "else if (";
    c.second->PrintVerilogDeclRaw(str);
    str += ") begin\n\t";
    if (par) { PrintScopeVar(par, str); }
    str += "state <= ";
    if (par) { PrintScopeParam(par, str); }
    str = str + "STATE_" + std::to_string(c.first->GetNum()) + ";\nend ";
  }

  return;
}

void CHPData::PrintVerilogAssignment(std::string &str) {
  std::string self = "self";
  std::string chan = "chan";
  std::string ch = "";
  if (printed) { return; }
  printed = 1;
  str += "\t\\";
  act_boolean_netlist_t *bnl;
  act_dynamic_var_t *dv = NULL;
  if (scope->GetProc()) {
    bnl = BOOL->getBNL(scope->GetProc());
    dv = BOOL->isDynamicRef(bnl, id);
  } else {
    get_chan_name(scope->GetChan(), ch);
  }

  char tmp[1024];
  if (!dv) { 
    id->sPrint(tmp,1024);
    if (tmp == self) {
      if (scope->GetDir() == 0) {
        str += "send_data";
      } else {
        str += "recv_data";
      }
    } else if (tmp == chan) {
      str += ch;
    } else {
      str += tmp;
    }
  } else {
    print_array_ref(id, scope, str);
  }
  str += " <= ";
  if (type == 0) {
    PrintExpression(u.assign.e, scope, str);
  } else if (type == 1 || type == 6) {
    str +=  "\\";
    if (scope->GetChan()) {
      str += ch;
    } else {
      u.recv.chan->sPrint(tmp,1024);
      str += tmp;
    }
  } else if (type == 2 || type == 5) {
    if (scope->GetChan()) {
      u.recv.chan->sPrint(tmp,1024);
      str += tmp;
    } else {
      PrintExpression(u.send.se, scope, str);
    }
  }
  str += " ;\n";

  return;
}

void CHPData::PrintVerilogRHS(std::string &str) {
  std::string ch = "";
  printed = 1;
  if (!scope->GetProc()) {
    get_chan_name(scope->GetChan(), ch);
  }

  char tmp[1024];

  if (type == 0) {
    PrintExpression(u.assign.e, scope, str);
  } else if (type == 1 || type == 6) {
    str +=  "\\";
    if (scope->GetChan()) {
      str += ch;
    } else {
      u.recv.chan->sPrint(tmp,1024);
      str += tmp;
    }
  } else if (type == 2 || type == 5) {
    if (scope->GetChan()) {
      u.recv.chan->sPrint(tmp,1024);
      str += tmp;
    } else {
      PrintExpression(u.send.se, scope, str);
    }
  } else if (type == 7) {
    str += "\\";
    u.send_struct.chan->sPrint(tmp,1024);
    str += tmp;
  }
  str += " ;\n";

  return;
}

void CHPData::PrintVerilogIfCond(std::string &da)
{

  if (type == 1 || type == 6) {
    u.recv.up_cond->PrintVerilogDeclRaw(da);
  } else if (type == 2 || type == 5) {
    u.send.up_cond->PrintVerilogDeclRaw(da);
  } else {
    cond->PrintVerilogDeclRaw(da);
  }

  return;
}

//f = 0 - if
//f = 1 - else if
//f = 2 - else
//f = 3 - combinational else
void CHPData::PrintVerilog(int f, std::string &da)
{
  std::string self = "self";
  std::string chan = "chan";
  std::string ch = "";
  char tmp[1024];

  if (f == 0) { da += "if ("; }
  else if (f == 1) { da += "else if ( "; }
  else if (f == 3) { da += "else "; }
  if (f == 0 || f == 1) {
    if (type == 1 || type == 6) {
      u.recv.up_cond->PrintVerilogDeclRaw(da);
    } else if (type == 2 || type == 5) {
      u.send.up_cond->PrintVerilogDeclRaw(da);
    } else {
      cond->PrintVerilogDeclRaw(da);
    }
    da += ") begin\n";
    PrintVerilogAssignment(da);
    da += "end ";
  }
  if (f == 2) {
    id->sPrint(tmp, 1024);
    if (scope->GetChan()) { get_chan_name(scope->GetChan(), ch); }
    da += "else begin\n\t";
    if (tmp == self) {
      da = da + "\\recv_data" + " <= \\recv_data ;\n";
    } else if (tmp == chan) {
      da = da + "\\" + ch + " <= \\" + ch + " ;\n";
    } else {
      da = da + "\\" + tmp + " <= \\" + tmp + " ;\n";
    }
    da += "end";
  }
  if (f == 3) {
    da += " begin\n";
    printed = 0;
    PrintVerilogAssignment(da);
    da += "end ";
  }

  return;
}

void StateMachine::PrintVerilogData() {
  int ef = 0;
  std::string da;
  char tmp[1024]; 

  for (auto id : test_data) {

    if (id.second[0]->GetType() == 2 || id.second[0]->GetType() == 7) {
      da += "always @(*)\n";
    } else {
      da += "always @(posedge \\clock )\n";
    }
  
    da = da + "if (\\reset ) begin\n\t\\" + id.first + " <= 0;\nend ";

    for (auto dd : id.second) {
      da += "else if ( ";
      dd->PrintVerilogIfCond(da);
      da += ") begin\n";
      da += "\t\\" + id.first + " <= ";
      dd->PrintVerilogRHS(da);
      da += "end ";

      if (dd == id.second[id.second.size()-1]) {
        if (id.second[0]->GetType() != 2 && id.second[0]->GetType() != 7) {
          da += "else begin\n";
          da += "\t\\" + id.first + " <= \\" + id.first + " ;\n";
          da += "end\n";
        } else {
          da += "else begin\n";
          da += "\t\\" + id.first + " <= ";
          id.second[0]->PrintVerilogRHS(da);
          da += "end\n";
        }
      }
    }
    fprintf(output_file, "%s\n\n", da.c_str());
    da.clear();
  }

  for (auto id : test_dyn_data) {
 
    bool comb = (id.second[0]->GetType() == 2 || id.second[0]->GetType() == 7);

    if (comb) {
      da += "always @(*)\n";
    } else {
      // the independent ifs below are several statements, so the block
      // needs an explicit begin/end
      da += "always @(posedge \\clock ) begin\n";
    }

    for (auto dd : id.second) {
      // clocked blocks emit independent ifs, one per array element,
      // combinational blocks emit an if/else-if chain
      if (!comb) {
        da += "if (";
      } else if (ef == 0) {
        da += "if (";
        ef = 1;
      } else {
        da += "else if (";
      }
      dd->PrintVerilogIfCond(da);
      da += ") begin\n";
      da += "\t\\";
      std::string dv_id = "";
      print_array_ref(dd->GetId(),this,dv_id);
      da += dv_id + " <= ";
      dd->PrintVerilogRHS(da);
      da += "end\n";
      if (dd == id.second[id.second.size()-1]) {
        if (id.second[0]->GetType() == 2) {
          da += "else begin\n";
          id.second[0]->PrintVerilogRHS(da);
          da += "end\n";
        }
        if (!comb) {
          da += "end\n";   // close the always block opened above
        }
        ef = 0;
      }
    }
 
    fprintf(output_file, "%s\n\n", da.c_str());
    da.clear();
  }



  return;
}

void CHPData::GetSuffix(std::string &str, int func) {

  Scope *s = act_scope->CurScope();
  act_connection *cid;
  
  char tmp[1024];
  
  if (type == 1 || type == 6) {
    cid = u.recv.chan->Canonical(s);
  } else if (type == 2 || type == 4 || type == 5) {
    if (id->isDynamicDeref()) {
      ActId *tmp;
      tmp = new ActId (id->getName(), NULL);
      cid = tmp->Canonical (s);
      delete tmp;
    }
    else {
      cid = id->Canonical(s);
    }
  }
  if (scope->IsPort(cid) == 0 && func == 0) {
    str = "_valid";
  } else if (scope->IsPort(cid) == 0 && func == 1) {
    str = "_ready";
  } else if (scope->IsPort(cid) == 1) {
    str = "_valid";
  } else if (scope->IsPort(cid) == 2) {
    str = "_ready";
  } else {
    str = "Whaaaat?!" + std::to_string(scope->IsPort(cid)) + "\n";
  }

  return;
}

void CHPData::PrintVerilogHSrhs(std::string &str){

  if (type == 1 || type == 6) {
    u.recv.up_cond->PrintVerilogDeclRaw(str);
  } else if (type == 2 || type == 5) {
    u.send.up_cond->PrintVerilogDeclRaw(str);
  }

  return;
}

void StateMachine::PrintVerilogDataHS()
{
  std::string hs;

  std::string suf0 = "";
  std::string suf1 = "";

  for (auto id : test_hs_data) {
    id.second[0]->GetSuffix(suf0, 0);
    id.second[0]->GetSuffix(suf1, 1);
    if (id.second[0]->GetType() == 5 || 
        id.second[0]->GetType() == 6) {
      int func = id.second[0]->GetType() == 5 ? 0 : 1;
      for (auto i = 0; i < 2; i++) {
        hs += "always @(posedge \\clock )\nif (reset) begin\n\t\\";
        hs += id.first;
        hs += func == 0 ? suf0 : suf1;
        hs += " <= 0 ;\n";
        hs += "end else if (\\";
        hs += id.first + suf0;
        hs += " & \\";
        hs += id.first + suf1;
        hs += " ) begin\n\t\\";
        hs += id.first;
        hs += func == 0 ? suf0 : suf1;
        hs += " <= 0 ;\n";
        for (auto dd : id.second) {
          if (func == 1 && dd->GetType() == 5 || 
              func == 0 && dd->GetType() == 6) {
            continue;
          }
          hs += "end else if (";
          dd->PrintVerilogHSrhs(hs);
          hs += ") begin\n\t\\";
          hs += id.first;
          hs += func == 0 ? suf0 : suf1;
          hs += " <= 1'b1 ;\n";
        }
        hs += "end else begin\n\t\\";
        hs += id.first;
        hs += func == 0 ? suf0 : suf1;
        hs += " <= \\";
        hs += id.first;
        hs += func == 0 ? suf0 : suf1;
        hs += " ;\nend\n\n";
        func = id.second[0]->GetType() == 5 ? 1 : 0;
      }
    } else {
      hs += "always @(*) begin\n\t\\";
      hs += id.first + suf0;
      hs += " <= (";
      for (auto dd : id.second) {
        if (dd->GetType() == 1 || 
            dd->GetType() == 2 ||
            dd->GetType() == 7) {
          dd->PrintVerilogHSrhs(hs);
          if (dd != id.second[id.second.size()-1]) { hs += " | "; }
        }
      }
      hs += " ) & ~reset ;\nend\n\n ";
    }
    fprintf(output_file, "%s", hs.c_str());
    hs.clear();
  }

  return;
}

void Port::PrintName(std::string &str){

  char tmp[1024];

  port_name->sPrint(tmp,1024);
  str += tmp;

  return;
}

void StateMachineInst::PrintVerilog(){

  if (glue == 1) {
    PrintAsGlue();
    return;
  };

  std::string inst = "\\";
  get_module_name(p, inst);
  if (glue == 0) {
    inst = inst + " \\" + u.p.name->getName();
    if (array) {
      inst += array;
    }
  } else {
    char tmp[1024];
    ports_c[0]->toid()->sPrint(tmp,1024);
    std::string glue_name = "";
    glue_name = glue_name + "\\" + u.g.name_src->getName() + "_" +
                      u.g.name_dst->getName() + "_" +
                      tmp;
  }
  inst += " (\n";
  inst += "\t .\\clock (\\clock )\n";
  if (sm->GetPrs() == 0) {
    inst += "\t,.\\reset (\\reset )\n";
  }

  for (auto i = 0; i < ports.size(); i++) {
    if (ports[i]->GetChan() != 2 && ports[i]->GetWidth() > 0) {
      inst += "\t,.\\";
      sm->GetPorts()[i]->PrintName(inst);
      inst += " (\\";
      ports[i]->PrintName(inst);
      inst += " )\n";
    }
    if (ports[i]->GetChan() != 0) {
      inst += "\t,.\\";
      sm->GetPorts()[i]->PrintName(inst);
      inst += "_ready (\\";
      ports[i]->PrintName(inst);
      inst += "_ready )\n";
      inst += "\t,.\\";
      sm->GetPorts()[i]->PrintName(inst);
      inst += "_valid (\\";
      ports[i]->PrintName(inst);
      inst += "_valid )\n";
    }
  }

  if (ports.size() < sm->GetPorts().size()) {
    for (auto i = ports.size(); i < sm->GetPorts().size(); i++) {
      inst += "\t,.\\";
      sm->GetPorts()[i]->PrintName(inst);
      inst += " (\\";
      sm->GetPorts()[i]->PrintName(inst);
      inst += " )\n";
    }
  }
  inst += ");\n\n";
  fprintf(output_file, "%s", inst.c_str());
  return;
}

void StateMachine::PrintVerilogBody()
{
  std::string sm;
  if (top) {
    sm += "always @(posedge \\clock )\n";
    sm += "if (\\reset ) begin\n\t";
    sm = sm + "sm" + std::to_string(number) + "_";
    if (par) { PrintScopeVar(par, sm); }
    sm += "state <= ";
    sm += "SM" + std::to_string(number) + "_";
    if (par) { PrintScopeParam(par, sm); }
    sm += "STATE_0;\nend ";
    top->PrintVerilog(sm);
    PrintVerilogState(top->GetNextState(), sm);
    sm += "else begin\n\tsm" + std::to_string(number) + "_";
    if (par) { PrintScopeVar(par,sm); }
    sm += "state <= sm"+ std::to_string(number) + "_";
    if (par) { PrintScopeVar(par,sm); }
    sm += "state;\nend\n";
  }
  fprintf(output_file, "%s\n", sm.c_str());
  sm.clear();

  return;
}

void StateMachine::PrintVerilog() {

  for (auto c : csm) {
    c->PrintVerilog();
  }

  for (auto sib : ssm) {
    sib->PrintVerilog();
  }

  std::string err;
  if (!top) {
    err += "/*\tNO CHP BODY IN THE PROCESS\t*/\n";
    fprintf(output_file, "%s", err.c_str());
  }
 
  std::string cond;
  for (auto cc : guard_condition) { cc->PrintVerilogExpr(cond); }
  for (auto cc : state_condition) { cc->PrintVerilogExpr(cond); }
  for (auto cc : commu_condition) { cc->PrintVerilogExpr(cond); }
  for (auto cc : comma_condition) { cc->PrintVerilogExpr(cond); }
  fprintf(output_file, "%s\n", cond.c_str());
  cond.clear();

  for (int ii = 0; ii < arb.size(); ii++) { arb[ii]->PrintInst(ii); }

  if (top) { PrintVerilogBody(); } 

  int has_comm = 0;
  int first = 0;
 
  int ef = 1;

  PrintVerilogData();
  PrintVerilogDataHS();

  PrintVerilogGlueData();

  for (auto i : inst) { 
    i->PrintVerilog(); 
  }
 
  if (!par && (number == 0)) {
    fprintf(output_file, "\nendmodule\n\n");
  }

  return; 
}

void StateMachine::PrintVerilogGlueData() {

  int ef = 0;
  std::string da;
  char tmp[1024]; 

  for (auto dd : glue_data) {

    std::string self = "self";
    std::string ch = "";
    get_chan_name(this->ch, ch);
    dd->GetId()->sPrint(tmp,1024);

    if (glue_dir == 1) {
      if (ports[0]->GetWidth() > 0) {
        da += "always @(posedge \\clock )\n";
        da = da + "if (\\reset ) begin\n\t\\" + ch + " <= 0;\nend ";
        dd->PrintVerilog(1, da);
        if (dd->GetType() != 2) {
          dd->PrintVerilog(2, da);
        } else {
          dd->PrintVerilog(3,da);
        }
        da += "\n";
      }
    } else {
      da += "always @(posedge \\clock )\n";
      da = da + "if (\\reset ) begin\n\t\\" + tmp + " <= 0;\nend ";
      if (ports[0]->GetWidth() > 0) { dd->PrintVerilog(1, da); }
      if (dd->GetType() != 2) {
        dd->PrintVerilog(2, da);
      } else {
        dd->PrintVerilog(3,da);
      }
      da += "\n";
    }
    da += "\n";

    da += "always @(*) begin\n\t\\";
    if (dd->GetType() == 1) {
      da = da + ch + "_ready";
    } else {
      da = da + ch + "_valid";
    }
    da += " <= (";
    dd->PrintVerilogHSrhs(da);
    da += " ) & ~reset ;\nend\n ";

    fprintf(output_file, "%s", da.c_str());
    da.clear();

  }

  return;
}

void Port::PrintAsGlue(std::string &tmp) {
  char port[1024];
  glue_port_name->sPrint(port,1024);
  if (dir == 0) {
    tmp = tmp + "\t,output\treg\t\\" + port;
  } else {
    tmp = tmp + "\t,input\t\t\\" + port;
  }
  tmp = tmp + "\n";

  return;
}

void StateMachineInst::PrintAsGlue() {

  std::string inst = "";
  char port[1024];

  std::string chan = "";
  get_chan_name(dynamic_cast<Channel*>(ch->BaseType()), chan);

  ports[0]->GetCon()->toid()->sPrint(port,1024);

  inst = inst + "\\" + chan;
  chan = "";
  if (dir == 0) { inst = inst + "_out "; }
  else { inst = inst + "_in "; }
  inst = inst + "\\" + u.g.name_src->getName() + "_" + u.g.name_dst->getName() + "_" + port + "\n";

  inst = inst + "(\n\t .\\clock\t(\\clock )\n";
  inst = inst + "\t,.\\reset\t(\\reset )\n";

  get_chan_name(sm->GetChan(), chan);
  if (ports[0]->GetWidth() > 0) {
    inst = inst + "\t,.\\" + chan + " (\\" + port + " )\n";
  }
  if (ports[0]->GetChan() != 0) {
    inst = inst + "\t,.\\" + chan + "_valid (\\" + port + "_valid )\n";
    inst = inst + "\t,.\\" + chan + "_ready (\\" + port + "_ready )\n";
  }

  for (auto i = 1; i < sm->GetPorts().size(); i++) {
    char tmp1[1024];
    sm->GetPorts()[i]->GetId()->sPrint(tmp1, 1024);
    inst = inst + "\t,.\\" + tmp1 + " (\\" + port + "." + tmp1 + " )\n";
  }
  inst = inst + ");";

  fprintf(output_file, "%s\n", inst.c_str());
  inst.clear();

  return;
}

void StateMachine::PrintAsGlue(std::string &ch_name) {

  std::string head = "";
  std::string tail = "";
  std::string tmp = "";

  get_chan_name(ch, tmp);

  head = head + "module \\" + ch_name + "\n(";
  head = head + "\t input\tclock\n\t,input\treset\n";
  if (glue_dir == 0) {
    head = head + "\t,input\t\\" + tmp + "_valid\n\t,output\treg\t\\" + tmp + "_ready\n";
    if (ports[0]->GetWidth() > 0) {
      head = head + "\t,input\t[" + std::to_string(ports[0]->GetWidth()-1) + ":0]\t\\" + tmp + "\n";
    }
  } else {
    head = head + "\t,output\treg\t\\" + tmp + "_valid\n\t,input\t\\" + tmp + "_ready\n";
    if (ports[0]->GetWidth() > 0) {
      head = head + "\t,output\treg\t[" + std::to_string(ports[0]->GetWidth()-1) + ":0]\t\\" + tmp + "\n";
    }
  }
  for (auto i = 1; i < ports.size(); i++) {
    ports[i]->PrintAsGlue(head);
  }
  head = head + ");\n";
  fprintf(output_file, "%s\n", head.c_str());

  PrintVerilogWires();
  for (auto s : ssm) { s->PrintVerilogWires(); }
  PrintVerilogVars();
  for (auto s : ssm) { s->PrintVerilogVars();  }
  PrintVerilogParameters();
  for (auto s : ssm) { s->PrintVerilogParameters(); }

  std::string cond;
  for (auto cc : guard_condition) { cc->PrintVerilogExpr(cond); }
  for (auto cc : state_condition) { cc->PrintVerilogExpr(cond); }
  for (auto cc : commu_condition) { cc->PrintVerilogExpr(cond); }
  for (auto cc : comma_condition) { cc->PrintVerilogExpr(cond); }
  fprintf(output_file, "%s\n", cond.c_str());
  cond.clear();

  PrintVerilog();

  head.clear();
  tail.clear();

  return;
}

void CHPProject::PrintVerilog(int sv, std::filesystem::path &path) {

  std::filesystem::path mod_path;
  mod_path = path / "func.v";
  func_file = fopen(mod_path.c_str(), "w");
  for (auto n = hd; n; n = n->GetNext()) {
    if (n->GetPrs() == 1) { continue; }
    std::string mod_name;
    get_module_name(n->GetProc(),mod_name);
    mod_path = path / (mod_name + ".v");
    output_file = fopen(mod_path.c_str(),"w");
    if (!n->GetPar()) {
      n->PrintVerilogHeader(sv);
    }
    if (!n->GetChan()) {
      fprintf(output_file, "`include \"func.v\"\n\n");
    }
    n->PrintVerilog();
    fclose(output_file);
  }
  fclose(func_file);
  for (auto n = ghd; n; n = n->GetNext()) {
    std::string ch_name;
    get_chan_name(n->GetChan(),ch_name);
    if (n->GetDir() == 0) { ch_name += "_out"; }
    if (n->GetDir() == 1) { ch_name += "_in"; }
    std::filesystem::path ch_path = path / (ch_name + ".v");
    output_file = fopen(ch_path.c_str(),"w");
    n->PrintAsGlue(ch_name);
    fclose(output_file);
  }
  return;  
}

void Arbiter::PrintInst(int n) {

  std::string arb;

  unsigned long size = log2(a.size());
  unsigned long total = pow(2,size);
  if (total != a.size()) {
    size = size + 1;
    total = pow(2, size);
  }

  if (total > a.size()) { arb += "wire [" + std::to_string(total-a.size()-1) + ":0] placeholder_arb_" + std::to_string(n) + ";\n"; }

  arb += "rr #(\n\t.W(" + std::to_string(size) + ")\n";
  arb += ") arb_" + std::to_string(n) + " (\n";
  arb += "\t .\\clock (\\clock )\n\t,.\\reset (\\reset )\n\t,.req ({";

  for (auto i = 0; i < total - a.size(); i++) { arb += "1'b0,"; }
  for (auto i : a) {
    arb += "\\";
    i->PrintScopeVar(i->GetScope(), arb);
    arb = arb + "excl_guard_" + std::to_string(i->GetNum());
    if (i != a[a.size()-1]) {
      arb += " ,";
    }
  }
  arb += " })\n";

  arb += "\t,.gnt ({";
  if (total > a.size()) { arb += "placeholder_arb_" + std::to_string(n) + ", "; }
  for (auto i : a) {
    arb += "\\";
    i->PrintScopeVar(i->GetScope(), arb);
    arb = arb + "guard_" + std::to_string(i->GetNum());
    if (i != a[a.size()-1]) {
      arb += " ,";
    }
  }
  arb += " })\n";

  arb += ");\n\n";
  fprintf(output_file, "%s", arb.c_str());
  arb.clear();

  return;
}

void Arbiter::PrintArbiter(FILE *fout){
std::string arb;
arb += "`timescale 1ns/1ps\n";
arb += "module rr\n";
arb += "#(\n";
arb += "\tparameter W = 4\n";
arb += ")(\n";
arb += "\t input\t\t\t\t\\clock\n";
arb += "\t,input\t\t\t\treset\n";
arb += "\t,input\t[2**W-1:0]\treq\n";
arb += "\t,output\t[2**W-1:0]\tgnt\n";
arb += ");\n";
arb += "\n";
arb += "wire\t[2**W-1:0]\ttfpe\t[W:0][1:0];\n";
arb += "\n";
arb += "wire\t\t\t\thp_gnt;\n";
arb += "\n";
arb += "reg\t\t[2**W-1:0]\tmask;\n";
arb += "\n";
arb += "wire\t[2**W-1:0]\tth_gnt;\n";
arb += "\n";
arb += "wire\t[2**W-1:0]\treq_masked;\n";
arb += "\n";
arb += "always @(posedge \\clock )\n";
arb += "if (reset) mask <= 0;\n";
// Drop the winning bit so the next round admits only the strictly
// lower indices and the rotation advances
arb += "else       mask <= th_gnt & ~gnt;\n";
arb += "\n";
arb += "assign req_masked = req & mask;\n";
arb += "\n";
arb += "genvar i, j, k;\n";
arb += "generate\n";
arb += "for (j = 0; j < 2; j = j + 1) begin\n";
arb += "\tfor (k = 0; k < W+1; k = k + 1) begin\n";
arb += "\t\tfor (i = 0; i < 2**W; i = i + 1) begin\n";
arb += "\n";
arb += "\t\tif (k == 0) begin\n";
arb += "\t\t\tif (j == 0)\n";
arb += "\t\t\t\tif ((i % 2) == 1)   assign tfpe[k][j][i] = req_masked[i];\n";
arb += "\t\t\t\telse                assign tfpe[k][j][i] = req_masked[i] | req_masked[i+1];\n";
arb += "\t\t\telse\n";
arb += "\t\t\t\tif ((i % 2) == 1)   assign tfpe[k][j][i] = req[i];\n";
arb += "\t\t\t\telse                assign tfpe[k][j][i] = req[i] | req[i+1];\n";
arb += "\t\tend\n";
arb += "\n";
arb += "\t\telse if (k == W) begin\n";
arb += "\t\t\tif (i == 0 | i == (2**W-1)) assign tfpe[k][j][i] = tfpe[k-1][j][i];\n";
arb += "\t\t\telse if ((i % 2) == 1)      assign tfpe[k][j][i] = tfpe[k-1][j][i] | tfpe[k-1][j][i+1];\n";
arb += "\t\t\telse                        assign tfpe[k][j][i] = tfpe[k-1][j][i];\n";
arb += "\t\tend\n";
arb += "\n";
arb += "\t\telse begin\n";
arb += "\t\t\tif (((i % 2) == 1) | ((i > (2**W-2**k-1)))) assign tfpe[k][j][i] =  tfpe[k-1][j][i];\n";
arb += "\t\t\telse                                        assign tfpe[k][j][i] =  tfpe[k-1][j][i] | tfpe[k-1][j][i+(2**k)];\n";
arb += "\t\tend\n";
arb += "\n";
arb += "\t\tend\n";
arb += "\tend\n";
arb += "end\n";
arb += "endgenerate\n";
arb += "\n";
arb += "assign hp_gnt = |tfpe[W][0];\n";
arb += "assign th_gnt = hp_gnt ? tfpe[W][0] : tfpe[W][1];\n";
arb += "\n";
arb += "wire [2**W:0] pre_gnt;\n";
arb += "assign pre_gnt = {1'b0, th_gnt} ^ {th_gnt, 1'b1};\n";
arb += "assign gnt = pre_gnt[2**W:1];\n";
arb += "\n";
arb += "endmodule\n\n";

fprintf(fout, "%s", arb.c_str());
arb.clear();
return;
}

}
