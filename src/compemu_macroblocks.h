/**
 * Header file for the JIT macroblock emitter functions
 */

//Support compiling functions
void comp_opcode_init(const cpu_history* history, uae_u8 extension);
void comp_opcode_unsupported(uae_u16 opcode);

/* v18 (TODO 7.5): blocks over branches (see compile_block()) */
extern const cpu_history* comp_trace_next;
void comp_opcode_BCOND(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_DBCOND(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_DBF(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_BRA(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_BSR(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_JMPIMM(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_JSRIMM(const cpu_history* history, struct comptbl* props) REGPARAM;

/* v18 (TODO 7.3): FPU */
void comp_opcode_FBCOND(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_FGENREG(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_FGENMEM(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_FGENMEMUM(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_FGENMEMUP(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_FGENIMM(const cpu_history* history, struct comptbl* props) REGPARAM;
void comp_opcode_FNOP(const cpu_history* history, struct comptbl* props) REGPARAM;
BOOL comp_fpu_active(void);
BOOL comp_fpu_compilable(uae_u16 opcode, const uae_u16* location);
extern BOOL comp_ppc_has_fpu;
