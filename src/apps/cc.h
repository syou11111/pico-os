#ifndef PICO_OS_CC_H
#define PICO_OS_CC_H

int  cc_compile_file(const char *src_name, const char *out_name);
int  cc_run_file(const char *bin_name);
int  cc_compile_and_run(const char *src_name);
void cc_disasm_file(const char *bin_name);

#endif /* PICO_OS_CC_H */
