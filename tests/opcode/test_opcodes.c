#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define NAN_HEADER 0x7FF8000000000000ULL
typedef uint64_t vtx_value_t;
static vtx_value_t make_smi(int64_t v){return NAN_HEADER|((uint64_t)(v&0x0000FFFFFFFFFFFFULL)<<3);}
typedef struct{uint8_t*c;size_t l,cap;vtx_value_t*k;uint32_t kc,kcap;uint16_t ml;}bc_t;
static void bi(bc_t*b,uint16_t ml){b->cap=256;b->c=calloc(b->cap,1);b->l=0;b->kcap=16;b->k=calloc(b->kcap,8);b->kc=0;b->ml=ml;}
static void be(bc_t*b,uint8_t op){if(b->l>=b->cap){b->cap*=2;b->c=realloc(b->c,b->cap);}b->c[b->l++]=op;}
static void beo(bc_t*b,uint8_t op,uint16_t o){be(b,op);be(b,(o>>8)&0xFF);be(b,o&0xFF);}
static uint16_t bc(bc_t*b,int64_t v){if(b->kc>=b->kcap){b->kcap*=2;b->k=realloc(b->k,b->kcap*8);}b->k[b->kc]=make_smi(v);return b->kc++;}
static void bp(bc_t*b,size_t off,uint16_t v){b->c[off]=(v>>8)&0xFF;b->c[off+1]=v&0xFF;}
static void bw(bc_t*b,const char*fn){FILE*f=fopen(fn,"wb");uint32_t m=0x564F4243,v=2,cl=b->l,cc=b->kc;uint16_t ml=b->ml,ms=16;fwrite(&m,4,1,f);fwrite(&v,4,1,f);fwrite(&cl,4,1,f);fwrite(&cc,4,1,f);uint8_t h[4]={(ml>>8)&0xFF,ml&0xFF,(ms>>8)&0xFF,ms&0xFF};fwrite(h,1,4,f);for(uint32_t i=0;i<cc;i++){uint64_t x=b->k[i];fwrite(&x,8,1,f);}fwrite(b->c,1,b->l,f);fclose(f);}
enum{OP_HALT=0,OP_LOAD_LOCAL=2,OP_STORE_LOCAL=3,OP_LOAD_CONST_INT=6,OP_IADD=13,OP_ISUB=14,OP_IMUL=15,OP_IDIV=16,OP_IMOD=17,OP_ICMP_EQ=29,OP_ICMP_LT=31,OP_ICMP_GT=33,OP_GOTO=41,OP_IF_FALSE=43,OP_CALL_RUNTIME=66};
#define RT_P 4
static int tr=0,tp=0,tf=0;
static int run(const char*fn,char*out,size_t sz){char cmd[512];snprintf(cmd,sizeof(cmd),"/tmp/vortex %s 2>/dev/null",fn);FILE*p=popen(cmd,"r");if(!p)return -1;if(out){fgets(out,sz,p);char*n=strchr(out,'\n');if(n)*n='\0';}return pclose(p);}
#define CHECK(name,expected) \
    tr++;fprintf(stderr,"  [%d] %s ... ",tr,name); \
    if(strcmp(out,expected)==0){fprintf(stderr,"PASS\n");tp++;} \
    else{fprintf(stderr,"FAIL (got '%s', expected '%s')\n",out,expected);tf++;}
int main(void){
    fprintf(stderr,"=== VORTEX Opcode Test Suite ===\n\n");
    char out[256];
    /* 1. Add */{bc_t b;bi(&b,2);uint16_t a=bc(&b,3),c=bc(&b,5);beo(&b,OP_LOAD_CONST_INT,a);beo(&b,OP_LOAD_CONST_INT,c);be(&b,OP_IADD);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t1.vtbc");run("/tmp/t1.vtbc",out,sizeof(out));CHECK("3+5=8","8");}
    /* 2. Sub */{bc_t b;bi(&b,2);uint16_t a=bc(&b,10),c=bc(&b,3);beo(&b,OP_LOAD_CONST_INT,a);beo(&b,OP_LOAD_CONST_INT,c);be(&b,OP_ISUB);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t2.vtbc");run("/tmp/t2.vtbc",out,sizeof(out));CHECK("10-3=7","7");}
    /* 3. Mul */{bc_t b;bi(&b,2);uint16_t a=bc(&b,6),c=bc(&b,7);beo(&b,OP_LOAD_CONST_INT,a);beo(&b,OP_LOAD_CONST_INT,c);be(&b,OP_IMUL);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t3.vtbc");run("/tmp/t3.vtbc",out,sizeof(out));CHECK("6*7=42","42");}
    /* 4. Div */{bc_t b;bi(&b,2);uint16_t a=bc(&b,20),c=bc(&b,4);beo(&b,OP_LOAD_CONST_INT,a);beo(&b,OP_LOAD_CONST_INT,c);be(&b,OP_IDIV);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t4.vtbc");run("/tmp/t4.vtbc",out,sizeof(out));CHECK("20/4=5","5");}
    /* 5. Mod */{bc_t b;bi(&b,2);uint16_t a=bc(&b,17),c=bc(&b,5);beo(&b,OP_LOAD_CONST_INT,a);beo(&b,OP_LOAD_CONST_INT,c);be(&b,OP_IMOD);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t5.vtbc");run("/tmp/t5.vtbc",out,sizeof(out));CHECK("17%5=2","2");}
    /* 6. Neg */{bc_t b;bi(&b,2);uint16_t a=bc(&b,0),c=bc(&b,5);beo(&b,OP_LOAD_CONST_INT,a);beo(&b,OP_LOAD_CONST_INT,c);be(&b,OP_ISUB);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t6.vtbc");run("/tmp/t6.vtbc",out,sizeof(out));CHECK("0-5=-5","-5");}
    /* 7. LT */{bc_t b;bi(&b,2);uint16_t a=bc(&b,3),c=bc(&b,5);beo(&b,OP_LOAD_CONST_INT,a);beo(&b,OP_LOAD_CONST_INT,c);be(&b,OP_ICMP_LT);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t7.vtbc");run("/tmp/t7.vtbc",out,sizeof(out));CHECK("3<5=true","true");}
    /* 8. GT */{bc_t b;bi(&b,2);uint16_t a=bc(&b,5),c=bc(&b,3);beo(&b,OP_LOAD_CONST_INT,a);beo(&b,OP_LOAD_CONST_INT,c);be(&b,OP_ICMP_GT);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t8.vtbc");run("/tmp/t8.vtbc",out,sizeof(out));CHECK("5>3=true","true");}
    /* 9. EQ */{bc_t b;bi(&b,2);uint16_t a=bc(&b,5),c=bc(&b,5);beo(&b,OP_LOAD_CONST_INT,a);beo(&b,OP_LOAD_CONST_INT,c);be(&b,OP_ICMP_EQ);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t9.vtbc");run("/tmp/t9.vtbc",out,sizeof(out));CHECK("5==5=true","true");}
    /* 10. Vars */{bc_t b;bi(&b,3);uint16_t c10=bc(&b,10),c20=bc(&b,20),c1=bc(&b,1);beo(&b,OP_LOAD_CONST_INT,c10);beo(&b,OP_STORE_LOCAL,0);beo(&b,OP_LOAD_CONST_INT,c20);beo(&b,OP_STORE_LOCAL,1);beo(&b,OP_LOAD_LOCAL,0);beo(&b,OP_LOAD_CONST_INT,c1);be(&b,OP_IADD);beo(&b,OP_STORE_LOCAL,0);beo(&b,OP_LOAD_LOCAL,0);beo(&b,OP_LOAD_LOCAL,1);be(&b,OP_IADD);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t10.vtbc");run("/tmp/t10.vtbc",out,sizeof(out));CHECK("vars a+b=31","31");}
    /* 11. Loop */{bc_t b;bi(&b,3);uint16_t c0=bc(&b,0),c1=bc(&b,1),c100=bc(&b,100);beo(&b,OP_LOAD_CONST_INT,c0);beo(&b,OP_STORE_LOCAL,0);beo(&b,OP_LOAD_CONST_INT,c0);beo(&b,OP_STORE_LOCAL,1);beo(&b,OP_LOAD_CONST_INT,c100);beo(&b,OP_STORE_LOCAL,2);size_t lp=b.l;beo(&b,OP_LOAD_LOCAL,1);beo(&b,OP_LOAD_LOCAL,2);be(&b,OP_ICMP_LT);size_t fp=b.l+1;beo(&b,OP_IF_FALSE,0);beo(&b,OP_LOAD_LOCAL,0);beo(&b,OP_LOAD_LOCAL,1);be(&b,OP_IADD);beo(&b,OP_STORE_LOCAL,0);beo(&b,OP_LOAD_LOCAL,1);beo(&b,OP_LOAD_CONST_INT,c1);be(&b,OP_IADD);beo(&b,OP_STORE_LOCAL,1);beo(&b,OP_GOTO,(uint16_t)lp);bp(&b,fp,(uint16_t)b.l);beo(&b,OP_LOAD_LOCAL,0);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t11.vtbc");run("/tmp/t11.vtbc",out,sizeof(out));CHECK("sum(0..99)=4950","4950");}
    /* 12. Nested */{bc_t b;bi(&b,4);uint16_t c0=bc(&b,0),c1=bc(&b,1),c3=bc(&b,3),c4=bc(&b,4);beo(&b,OP_LOAD_CONST_INT,c0);beo(&b,OP_STORE_LOCAL,0);beo(&b,OP_LOAD_CONST_INT,c0);beo(&b,OP_STORE_LOCAL,1);size_t ol=b.l;beo(&b,OP_LOAD_LOCAL,1);beo(&b,OP_LOAD_CONST_INT,c3);be(&b,OP_ICMP_LT);size_t ofp=b.l+1;beo(&b,OP_IF_FALSE,0);beo(&b,OP_LOAD_CONST_INT,c0);beo(&b,OP_STORE_LOCAL,2);size_t il=b.l;beo(&b,OP_LOAD_LOCAL,2);beo(&b,OP_LOAD_CONST_INT,c4);be(&b,OP_ICMP_LT);size_t ifp=b.l+1;beo(&b,OP_IF_FALSE,0);beo(&b,OP_LOAD_LOCAL,0);beo(&b,OP_LOAD_CONST_INT,c1);be(&b,OP_IADD);beo(&b,OP_STORE_LOCAL,0);beo(&b,OP_LOAD_LOCAL,2);beo(&b,OP_LOAD_CONST_INT,c1);be(&b,OP_IADD);beo(&b,OP_STORE_LOCAL,2);beo(&b,OP_GOTO,(uint16_t)il);bp(&b,ifp,(uint16_t)b.l);beo(&b,OP_LOAD_LOCAL,1);beo(&b,OP_LOAD_CONST_INT,c1);be(&b,OP_IADD);beo(&b,OP_STORE_LOCAL,1);beo(&b,OP_GOTO,(uint16_t)ol);bp(&b,ofp,(uint16_t)b.l);beo(&b,OP_LOAD_LOCAL,0);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t12.vtbc");run("/tmp/t12.vtbc",out,sizeof(out));CHECK("nested 3*4=12","12");}
    /* 13. If/else */{bc_t b;bi(&b,2);uint16_t c5=bc(&b,5),c3=bc(&b,3),c1=bc(&b,1),c0=bc(&b,0);beo(&b,OP_LOAD_CONST_INT,c5);beo(&b,OP_LOAD_CONST_INT,c3);be(&b,OP_ICMP_GT);size_t fp=b.l+1;beo(&b,OP_IF_FALSE,0);beo(&b,OP_LOAD_CONST_INT,c1);beo(&b,OP_CALL_RUNTIME,RT_P);size_t gp=b.l+1;beo(&b,OP_GOTO,0);bp(&b,fp,(uint16_t)b.l);beo(&b,OP_LOAD_CONST_INT,c0);beo(&b,OP_CALL_RUNTIME,RT_P);bp(&b,gp,(uint16_t)b.l);be(&b,OP_HALT);bw(&b,"/tmp/t13.vtbc");run("/tmp/t13.vtbc",out,sizeof(out));CHECK("if/else 5>3->1","1");}
    /* 14. JIT sum */{bc_t b;bi(&b,3);uint16_t c0=bc(&b,0),c1=bc(&b,1),cN=bc(&b,100000);beo(&b,OP_LOAD_CONST_INT,c0);beo(&b,OP_STORE_LOCAL,0);beo(&b,OP_LOAD_CONST_INT,c0);beo(&b,OP_STORE_LOCAL,1);beo(&b,OP_LOAD_CONST_INT,cN);beo(&b,OP_STORE_LOCAL,2);size_t lp=b.l;beo(&b,OP_LOAD_LOCAL,1);beo(&b,OP_LOAD_LOCAL,2);be(&b,OP_ICMP_LT);size_t fp=b.l+1;beo(&b,OP_IF_FALSE,0);beo(&b,OP_LOAD_LOCAL,0);beo(&b,OP_LOAD_LOCAL,1);be(&b,OP_IADD);beo(&b,OP_STORE_LOCAL,0);beo(&b,OP_LOAD_LOCAL,1);beo(&b,OP_LOAD_CONST_INT,c1);be(&b,OP_IADD);beo(&b,OP_STORE_LOCAL,1);beo(&b,OP_GOTO,(uint16_t)lp);bp(&b,fp,(uint16_t)b.l);beo(&b,OP_LOAD_LOCAL,0);beo(&b,OP_CALL_RUNTIME,RT_P);be(&b,OP_HALT);bw(&b,"/tmp/t14.vtbc");run("/tmp/t14.vtbc",out,sizeof(out));CHECK("JIT sum(100K)=4999950000","4999950000");}
    fprintf(stderr,"\n=== Results: %d/%d passed, %d failed ===\n",tp,tr,tf);
    return tf>0?1:0;
}
