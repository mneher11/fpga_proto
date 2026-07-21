#include <stdio.h>
#include <unistd.h>
#include <filesystem>
#include <act/act.h>

#include "state_machine.h"

namespace fpga {
  Act *a;
  ActCHPFuncInline *INLINE;
  ActBooleanizePass *BOOL;
}

void logo () {

fprintf(stdout, "    +  +  +  +  +  +  +  +        +  +  +  +  +  +  +  +        +  +  +  +  +  +  +  +    \n");
fprintf(stdout, "  +-+--+--+--+--+--+--+--+-+    +-+--+--+--+--+--+--+--+-+    +-+--+--+--+--+--+--+--+-+  \n");
fprintf(stdout, "+-+                        +-++-+                        +-++-+                        +-+\n");
fprintf(stdout, "  |                        |    |                        |    |                        |  \n");
fprintf(stdout, "+-+                        +-++-+      Extended Ver.     +-++-+                        +-+\n");
fprintf(stdout, "  |                        |    |       CHP -> FPGA      |    |                        |  \n");
fprintf(stdout, "+-+                        +-++-+     Yale University    +-++-+                        +-+\n");
fprintf(stdout, "  |                        |    |        AVLSI Lab       |    |                        |  \n");
fprintf(stdout, "+-+                        +-++-+     Ruslan Dashkin     +-++-+                        +-+\n");
fprintf(stdout, "  |                        |    |      Rajit Manohar     |    |                        |  \n");
fprintf(stdout, "+-+                        +-++-+                        +-++-+                        +-+\n");
fprintf(stdout, "  |                        |    |                        |    |                        |  \n");
fprintf(stdout, "+-+                        +-++-+                        +-++-+                        +-+\n");
fprintf(stdout, "  +-+--+--+--+--+--+--+--+-+    +-+--+--+--+--+--+--+--+-+    +-+--+--+--+--+--+--+--+-+  \n");
fprintf(stdout, "    +  +  +  +  +  +  +  +        +  +  +  +  +  +  +  +        +  +  +  +  +  +  +  +    \n");

}

void usage () {
  fprintf(stdout, "=============================================================================================\n");
  logo();
  fprintf(stdout, "=============================================================================================\n");
  fprintf(stdout, "Usage: chp2fpga [-p <process_name>] <*.act>\n");
  fprintf(stdout, "=============================================================================================\n");
  fprintf(stdout, "h - Usage guide\n");
  fprintf(stdout, "p - Top process name\n");
  fprintf(stdout, "m - Decompose dynamic arrays into memories (needed for structure memories)\n");
  fprintf(stdout, "a - Add arbiter to the print out (only needed for non-det selection)\n");
  fprintf(stdout, "o - Relative output path (default current directory)\n");
  fprintf(stdout, "=============================================================================================\n");
}

int main (int argc, char **argv) { 

  char *proc = NULL;

  Act::Init(&argc, &argv);

  char *conf = NULL;
  //std::string out_path = std::filesystem::current_path();
  std::filesystem::path out_path = std::filesystem::current_path();

  int key = 0;

  extern int opterr;
  opterr = 0;

  int parb = 0;
  int sv = 0;
  int opt = 0;
  int mem_pass = 0;
  while ((key = getopt (argc, argv, "mp:asho:c:O:")) != -1) {
    switch (key) {
      case 'm':
        mem_pass = 1;
        break;
      case 'o':
        if (optarg == NULL) {
          printf("ERROR: MISSING OUTPUT FOLDER NAME\n");
          exit(1);
        } else {
	  std::filesystem::path arg (optarg);
          if (std::filesystem::is_directory(arg)) {
            if (arg.is_absolute()) {
	      out_path = optarg;
            }
            else {
	      out_path = std::filesystem::current_path() / arg;
            }
          } else {
            printf("ERROR: FOLDER DOES NOT EXIST\n");
            exit(1);
          }
        }
        break;
      case 'h':
        usage();
        exit(1);
        break;
      case 'a':
        parb = 1;
        break;
      case 'p':
        if (proc) {
          FREE(proc);
        }
        if (optarg == NULL) {
          fatal_error ("Missing process name");
        }
        proc = Strdup(optarg);
        break;
      case ':':
        fprintf(stderr, "Need a file here\n");
        break;
      case '?':
        usage();
        fatal_error("Something went wrong\n");
        break;
      default: 
        fatal_error("Hmm...\n");
        break;
    }
  }

  if (optind != argc - 1) {
    fatal_error("Missing act file name\n");
  }

  if (proc == NULL) {
    fatal_error("Missing process name\n");
  }
 
  fpga::a = new Act(argv[optind]);
  fpga::a->Expand ();

	Process *p = fpga::a->findProcess (proc);

	if (!p) {
		fatal_error ("Wrong process name, %s", proc);
	}

	if (!p->isExpanded()){
                p = p->Expand (ActNamespace::Global(), p->CurScope(), 0, NULL);
	}

	fpga::BOOL = new ActBooleanizePass (fpga::a);
	Assert (fpga::BOOL->run(p), "Booleanize pass failed");

        if (mem_pass) {
           ActCHPMemory *mem = new ActCHPMemory (fpga::a);
           mem->run (p);
        }

	fpga::INLINE = new ActCHPFuncInline (fpga::a);
	Assert (fpga::INLINE->run(p), "Function inline pass failed");
	fpga::INLINE->run(p);


  fpga::CHPProject *cp;

  cp = fpga::build_machine(p,opt,proc);
  
  if (parb == 1) {
    FILE *arb_file;
    std::filesystem::path arb_path = out_path / std::filesystem::path ("arbiter.v");
    arb_file = fopen(arb_path.c_str(), "w");
	  fpga::Arbiter *arb = new fpga::Arbiter();
	  arb->PrintArbiter(arb_file);
    fclose(arb_file);
  }
  
  cp->PrintVerilog(sv, out_path);

  printf("OUTPUT FILES ARE STORE IN THE FOLDER: %s\n", out_path.c_str());

  return 0;
}
