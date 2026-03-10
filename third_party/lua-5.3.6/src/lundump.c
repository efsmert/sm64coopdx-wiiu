/*
** $Id: lundump.c,v 2.44.1.1 2017/04/19 17:20:42 roberto Exp $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#define lundump_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>
#include <stdint.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstring.h"
#include "lundump.h"
#include "lzio.h"


#if !defined(luai_verifycode)
#define luai_verifycode(L,b,f)  /* empty */
#endif


typedef struct {
  lua_State *L;
  ZIO *Z;
  const char *name;
  lu_byte int_size;
  lu_byte size_t_size;
  lu_byte instruction_size;
  lu_byte lua_integer_size;
  lu_byte lua_number_size;
  lu_byte swap_endian;
} LoadState;


static l_noret error(LoadState *S, const char *why) {
  luaO_pushfstring(S->L, "%s: %s precompiled chunk", S->name, why);
  luaD_throw(S->L, LUA_ERRSYNTAX);
}


/*
** All high-level loads go through LoadVector; you can change it to
** adapt to the endianness of the input
*/
#define LoadVector(S,b,n)	LoadVectorSized(S,b,n,sizeof((b)[0]))

static void LoadBlock (LoadState *S, void *b, size_t size) {
  if (luaZ_read(S->Z, b, size) != 0)
    error(S, "truncated");
}

static void LoadVectorSized(LoadState *S, void *b, size_t n, size_t elemSize) {
  LoadBlock(S, b, n * elemSize);
  if (S->swap_endian && elemSize > 1) {
    unsigned char *p = (unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
      unsigned char *e = p + (i * elemSize);
      for (size_t j = 0; j < elemSize / 2; j++) {
        unsigned char tmp = e[j];
        e[j] = e[elemSize - 1 - j];
        e[elemSize - 1 - j] = tmp;
      }
    }
  }
}


#define LoadVar(S,x)		LoadVector(S,&x,1)

static int lundump_host_is_little_endian(void) {
  const uint16_t one = 1;
  return *((const uint8_t*)&one) == 1;
}

static uint64_t lundump_read_uint(const unsigned char *buf, size_t size, int little_endian) {
  uint64_t out = 0;
  if (little_endian) {
    for (size_t i = 0; i < size; i++) {
      out |= ((uint64_t)buf[i]) << (i * 8);
    }
  } else {
    for (size_t i = 0; i < size; i++) {
      out = (out << 8) | (uint64_t)buf[i];
    }
  }
  return out;
}

static size_t LoadSize (LoadState *S) {
  unsigned char buff[8];
  uint64_t value;
  size_t max_size_t;

  if (S->size_t_size == 0 || S->size_t_size > sizeof(buff)) {
    error(S, "size_t size mismatch in");
  }

  LoadBlock(S, buff, S->size_t_size);
  value = lundump_read_uint(buff, S->size_t_size, lundump_host_is_little_endian() ? !S->swap_endian : S->swap_endian);
  max_size_t = (size_t)(~(size_t)0);
  if (value > (uint64_t)max_size_t) {
    error(S, "size_t overflow in");
  }
  return (size_t)value;
}


static lu_byte LoadByte (LoadState *S) {
  lu_byte x;
  LoadVar(S, x);
  return x;
}


static int LoadInt (LoadState *S) {
  int x;
  LoadVar(S, x);
  return x;
}


static lua_Number LoadNumber (LoadState *S) {
  lua_Number x;
  LoadVar(S, x);
  return x;
}


static lua_Integer LoadInteger (LoadState *S) {
  lua_Integer x;
  LoadVar(S, x);
  return x;
}


static TString *LoadString (LoadState *S, Proto *p) {
  lua_State *L = S->L;
  size_t size = LoadByte(S);
  TString *ts;
  if (size == 0xFF)
    size = LoadSize(S);
  if (size == 0)
    return NULL;
  else if (--size <= LUAI_MAXSHORTLEN) {  /* short string? */
    char buff[LUAI_MAXSHORTLEN];
    LoadVector(S, buff, size);
    ts = luaS_newlstr(L, buff, size);
  }
  else {  /* long string */
    ts = luaS_createlngstrobj(L, size);
    setsvalue2s(L, L->top, ts);  /* anchor it ('loadVector' can GC) */
    luaD_inctop(L);
    LoadVector(S, getstr(ts), size);  /* load directly in final place */
    L->top--;  /* pop string */
  }
  luaC_objbarrier(L, p, ts);
  return ts;
}


static void LoadCode (LoadState *S, Proto *f) {
  int n = LoadInt(S);
  f->code = luaM_newvector(S->L, n, Instruction);
  f->sizecode = n;
  LoadVector(S, f->code, n);
}


static void LoadFunction(LoadState *S, Proto *f, TString *psource);


static void LoadConstants (LoadState *S, Proto *f) {
  int i;
  int n = LoadInt(S);
  f->k = luaM_newvector(S->L, n, TValue);
  f->sizek = n;
  for (i = 0; i < n; i++)
    setnilvalue(&f->k[i]);
  for (i = 0; i < n; i++) {
    TValue *o = &f->k[i];
    int t = LoadByte(S);
    switch (t) {
    case LUA_TNIL:
      setnilvalue(o);
      break;
    case LUA_TBOOLEAN:
      setbvalue(o, LoadByte(S));
      break;
    case LUA_TNUMFLT:
      setfltvalue(o, LoadNumber(S));
      break;
    case LUA_TNUMINT:
      setivalue(o, LoadInteger(S));
      break;
    case LUA_TSHRSTR:
    case LUA_TLNGSTR:
      setsvalue2n(S->L, o, LoadString(S, f));
      break;
    default:
      lua_assert(0);
    }
  }
}


static void LoadProtos (LoadState *S, Proto *f) {
  int i;
  int n = LoadInt(S);
  f->p = luaM_newvector(S->L, n, Proto *);
  f->sizep = n;
  for (i = 0; i < n; i++)
    f->p[i] = NULL;
  for (i = 0; i < n; i++) {
    f->p[i] = luaF_newproto(S->L);
    luaC_objbarrier(S->L, f, f->p[i]);
    LoadFunction(S, f->p[i], f->source);
  }
}


static void LoadUpvalues (LoadState *S, Proto *f) {
  int i, n;
  n = LoadInt(S);
  f->upvalues = luaM_newvector(S->L, n, Upvaldesc);
  f->sizeupvalues = n;
  for (i = 0; i < n; i++)
    f->upvalues[i].name = NULL;
  for (i = 0; i < n; i++) {
    f->upvalues[i].instack = LoadByte(S);
    f->upvalues[i].idx = LoadByte(S);
  }
}


static void LoadDebug (LoadState *S, Proto *f) {
  int i, n;
  n = LoadInt(S);
  f->lineinfo = luaM_newvector(S->L, n, int);
  f->sizelineinfo = n;
  LoadVector(S, f->lineinfo, n);
  n = LoadInt(S);
  f->locvars = luaM_newvector(S->L, n, LocVar);
  f->sizelocvars = n;
  for (i = 0; i < n; i++)
    f->locvars[i].varname = NULL;
  for (i = 0; i < n; i++) {
    f->locvars[i].varname = LoadString(S, f);
    f->locvars[i].startpc = LoadInt(S);
    f->locvars[i].endpc = LoadInt(S);
  }
  n = LoadInt(S);
  for (i = 0; i < n; i++)
    f->upvalues[i].name = LoadString(S, f);
}


static void LoadFunction (LoadState *S, Proto *f, TString *psource) {
  f->source = LoadString(S, f);
  if (f->source == NULL)  /* no source in dump? */
    f->source = psource;  /* reuse parent's source */
  f->linedefined = LoadInt(S);
  f->lastlinedefined = LoadInt(S);
  f->numparams = LoadByte(S);
  f->is_vararg = LoadByte(S);
  f->maxstacksize = LoadByte(S);
  LoadCode(S, f);
  LoadConstants(S, f);
  LoadUpvalues(S, f);
  LoadProtos(S, f);
  LoadDebug(S, f);
}


static void checkliteral (LoadState *S, const char *s, const char *msg) {
  char buff[sizeof(LUA_SIGNATURE) + sizeof(LUAC_DATA)]; /* larger than both */
  size_t len = strlen(s);
  LoadVector(S, buff, len);
  if (memcmp(s, buff, len) != 0)
    error(S, msg);
}


static void fchecksize (LoadState *S, size_t size, const char *tname) {
  if (LoadByte(S) != size)
    error(S, luaO_pushfstring(S->L, "%s size mismatch in", tname));
}


#define checksize(S,t)	fchecksize(S,sizeof(t),#t)

static void checkHeader (LoadState *S) {
  unsigned char intbuff[16];
  unsigned char numbuff[16];
  int host_little;
  int chunk_little;

  checkliteral(S, LUA_SIGNATURE + 1, "not a");  /* 1st char already checked */
  if (LoadByte(S) != LUAC_VERSION)
    error(S, "version mismatch in");
  if (LoadByte(S) != LUAC_FORMAT)
    error(S, "format mismatch in");
  checkliteral(S, LUAC_DATA, "corrupted");

  S->int_size = LoadByte(S);
  S->size_t_size = LoadByte(S);
  S->instruction_size = LoadByte(S);
  S->lua_integer_size = LoadByte(S);
  S->lua_number_size = LoadByte(S);

  if (S->int_size != sizeof(int))
    error(S, luaO_pushfstring(S->L, "%s size mismatch in", "int"));
  if (S->instruction_size != sizeof(Instruction))
    error(S, luaO_pushfstring(S->L, "%s size mismatch in", "Instruction"));
  if (S->lua_integer_size != sizeof(lua_Integer))
    error(S, luaO_pushfstring(S->L, "%s size mismatch in", "lua_Integer"));
  if (S->lua_number_size != sizeof(lua_Number))
    error(S, luaO_pushfstring(S->L, "%s size mismatch in", "lua_Number"));
  if (S->size_t_size != sizeof(size_t) && S->size_t_size != 4 && S->size_t_size != 8)
    error(S, luaO_pushfstring(S->L, "%s size mismatch in", "size_t"));

  if (S->lua_integer_size > sizeof(intbuff))
    error(S, "lua_Integer size mismatch in");
  LoadBlock(S, intbuff, S->lua_integer_size);

  if (lundump_read_uint(intbuff, S->lua_integer_size, 1) == (uint64_t)LUAC_INT) {
    chunk_little = 1;
  } else if (lundump_read_uint(intbuff, S->lua_integer_size, 0) == (uint64_t)LUAC_INT) {
    chunk_little = 0;
  } else {
    error(S, "endianness mismatch in");
  }

  host_little = lundump_host_is_little_endian();
  S->swap_endian = (lu_byte)(host_little != chunk_little);

  if (S->lua_number_size > sizeof(numbuff))
    error(S, "lua_Number size mismatch in");
  LoadBlock(S, numbuff, S->lua_number_size);
  if (S->swap_endian && S->lua_number_size > 1) {
    for (size_t j = 0; j < S->lua_number_size / 2; j++) {
      unsigned char tmp = numbuff[j];
      numbuff[j] = numbuff[S->lua_number_size - 1 - j];
      numbuff[S->lua_number_size - 1 - j] = tmp;
    }
  }
  lua_Number n;
  memcpy(&n, numbuff, sizeof(n));
  if (n != LUAC_NUM)
    error(S, "float format mismatch in");
}


/*
** load precompiled chunk
*/
LClosure *luaU_undump(lua_State *L, ZIO *Z, const char *name) {
  LoadState S;
  LClosure *cl;
  S.swap_endian = 0;
  if (*name == '@' || *name == '=')
    S.name = name + 1;
  else if (*name == LUA_SIGNATURE[0])
    S.name = "binary string";
  else
    S.name = name;
  S.L = L;
  S.Z = Z;
  checkHeader(&S);
  cl = luaF_newLclosure(L, LoadByte(&S));
  setclLvalue(L, L->top, cl);
  luaD_inctop(L);
  cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  LoadFunction(&S, cl->p, NULL);
  lua_assert(cl->nupvalues == cl->p->sizeupvalues);
  luai_verifycode(L, buff, cl->p);
  return cl;
}
