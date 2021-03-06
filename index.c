/*
 * This file implements the indexing logic of Alsosql
 *

MIT License

Copyright (c) 2010 Russell Sullivan <jaksprats AT gmail DOT com>
ALL RIGHTS RESERVED 

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>

#include "adlist.h"
#include "redis.h"

#include "bt.h"
#include "btreepriv.h"
#include "bt_iterator.h"
#include "row.h"
#include "common.h"
#include "alsosql.h"
#include "rdb_alsosql.h"
#include "orderby.h"
#include "index.h"

// FROM redis.c
#define RL4 redisLog(4,
extern struct sharedObjectsStruct shared;
extern struct redisServer server;

extern char     CMINUS;
extern char     CPERIOD;
extern char    *Col_type_defs[];
extern r_tbl_t  Tbl[MAX_NUM_DB][MAX_NUM_TABLES];

// GLOBALS
int     Num_indx[MAX_NUM_DB];
r_ind_t Index   [MAX_NUM_DB][MAX_NUM_INDICES];

// HELPER_COMMANDS HELPER_COMMANDS HELPER_COMMANDS HELPER_COMMANDS
// HELPER_COMMANDS HELPER_COMMANDS HELPER_COMMANDS HELPER_COMMANDS
int find_index(int tmatch, int cmatch) {
    for (int i = 0; i < Num_indx[server.dbid]; i++) {
        if (Index[server.dbid][i].obj) {
            if (Index[server.dbid][i].table  == tmatch &&
                Index[server.dbid][i].column == cmatch) {
                return i;
            }
        }
    }
    return -1;
}

int match_index(int tmatch, int indices[]) {
    int matches = 0;
    for (int i = 0; i < Num_indx[server.dbid]; i++) {
        if (Index[server.dbid][i].obj) {
            if (Index[server.dbid][i].table == tmatch) {
                indices[matches] = i;
                matches++;
            }
        }
    }
    return matches;
}

int match_index_name(char *iname) {
    for (int i = 0; i < Num_indx[server.dbid]; i++) {
        if (Index[server.dbid][i].obj) {
            if (!strcmp(iname, (char *)Index[server.dbid][i].obj->ptr)) {
                return i;
            }
        }
    }
    return -1;
}

int checkIndexedColumnOrReply(redisClient *c, char *curr_tname) {
    char *nextp = strchr(curr_tname, CPERIOD);
    if (!nextp) {
        addReply(c, shared.badindexedcolumnsyntax);
        return -1;
    }
    *nextp = '\0';
    TABLE_CHECK_OR_REPLY(curr_tname, -1)
    nextp++;
    COLUMN_CHECK_OR_REPLY(nextp, -1)
    int imatch = find_index(tmatch, cmatch);
    if (imatch == -1) {
        addReply(c, shared.nonexistentindex);
        return -1;
    }
    return imatch;
}

/* NON_RELATIONAL_INDEX NON_RELATIONAL_INDEX NON_RELATIONAL_INDEX */
/* NON_RELATIONAL_INDEX NON_RELATIONAL_INDEX NON_RELATIONAL_INDEX */
sds genNRL_Cmd(d_l_t  *nrlind,
               robj   *pko,
               char   *vals,
               uint32  cofsts[],
               bool    from_insert,
               robj   *row,
               int     tmatch) {
        sds       cmd     = sdsempty();
        list     *nrltoks = nrlind->l1;
        list     *nrlcols = nrlind->l2;
        listIter *li1     = listGetIterator(nrltoks, AL_START_HEAD);
        listIter *li2     = listGetIterator(nrlcols, AL_START_HEAD);
        listNode *ln1     = listNext(li1);
        listNode *ln2     = listNext(li2);
        while (ln1 || ln2) {
            if (ln1) {
                sds token = ln1->value;
                cmd       = sdscatlen(cmd, token, sdslen(token));
            }
            int cmatch = -1;
            if (ln2) {
                cmatch = (int)(long)ln2->value;
                cmatch--; /* because (0 != NULL) */
            }
            if (cmatch != -1) {
                char *x;
                int   xlen;
                robj *col = NULL;
                if (from_insert) {
                    if (!cmatch) {
                        x    = pko->ptr;
                        xlen = sdslen(x);
                    } else {
                        x    = vals + cofsts[cmatch - 1];
                        xlen = cofsts[cmatch] - cofsts[cmatch - 1] - 1;
                    }
                } else {
                    col = createColObjFromRow(row, cmatch, pko, tmatch);
                    x    = col->ptr;
                    xlen = sdslen(col->ptr);
                }
                cmd = sdscatlen(cmd, x, xlen);
                if (col) decrRefCount(col);
            }
            ln1 = listNext(li1);
            ln2 = listNext(li2);
        }
    return cmd;
}

void runCmdInFakeClient(sds s) {
    char *end = strchr(s, ' ');
    if (!end) return;

    sds   *argv    = NULL; /* must come before first GOTO */
    int    a_arity = 0;
    sds cmd_name   = sdsnewlen(s, end - s);
    end++;
    struct redisCommand *cmd = lookupCommand(cmd_name);
    if (!cmd) goto run_cmd_err;
    int arity = abs(cmd->arity);

    char *args = strchr(end, ' ');
    if (!args) goto run_cmd_err;
    args++;

    argv                = malloc(sizeof(sds) * arity);
    argv[0]             = cmd_name;
    a_arity++;
    argv[1]             = sdsnewlen(end, args - end - 1);
    a_arity++;
    if (arity == 3) {
        argv[2]         = sdsnewlen(args, strlen(args));
        a_arity++;
    } else if (arity > 3) {
        char *dlm       = strchr(args, ' ' );;
        if (!dlm) goto run_cmd_err;
        dlm++;
        argv[2]         = sdsnewlen(args, dlm - args - 1);
        a_arity++;
        if (arity == 4) {
            argv[3]     = sdsnewlen(dlm, strlen(dlm));
            a_arity++;
        } else { /* INSERT */
            char *vlist = strchr(dlm, ' ' );;
            if (!vlist) goto run_cmd_err;
            vlist++;
            argv[3]     = sdsnewlen(dlm, vlist - dlm - 1);
            a_arity++;
            argv[4]     = sdsnewlen(vlist, strlen(vlist));
            a_arity++;
        }
    }

    robj **rargv = malloc(sizeof(robj *) * arity);
    for (int j = 0; j < arity; j++) {
        rargv[j] = createObject(REDIS_STRING, argv[j]);
    }
    redisClient *fc = rsql_createFakeClient();
    fc->argv        = rargv;
    fc->argc        = arity;
    call(fc, cmd);
    rsql_freeFakeClient(fc);
    free(rargv);

run_cmd_err:
    if (!a_arity) sdsfree(cmd_name);
    if (argv)     free(argv);
}

static void nrlIndexAdd(robj *o, robj *pko, char *vals, uint32 cofsts[]) {
    sds cmd = genNRL_Cmd(o->ptr, pko, vals, cofsts, 1, NULL, -1);
    runCmdInFakeClient(cmd);
    sdsfree(cmd);
    return;
}
/* INDEX_MAINTENANCE INDEX_MAINTENANCE INDEX_MAINTENANCE INDEX_MAINTENANCE */
/* INDEX_MAINTENANCE INDEX_MAINTENANCE INDEX_MAINTENANCE INDEX_MAINTENANCE */
void iAdd(bt *ibtr, robj *i_key, robj *i_val, uchar pktype) {
    bt   *nbtr;
    robj *nbt = btIndFindVal(ibtr, i_key, ibtr->ktype);
    if (!nbt) {
        nbt  = createIndexNode(pktype);
        btIndAdd(ibtr, i_key, nbt, ibtr->ktype);
        nbtr = (bt *)(nbt->ptr);
        ibtr->malloc_size += nbtr->malloc_size; /* ibtr inherits nbtr */
    } else {
        nbtr = (bt *)(nbt->ptr);
    }
    ull pre_size  = nbtr->malloc_size;
    btIndNodeAdd(nbtr, i_val, pktype);
    ull post_size = nbtr->malloc_size;
    ibtr->malloc_size += (post_size - pre_size); /* ibtr inherits nbtr */
}

static void iRem(bt *ibtr, robj *i_key, robj *i_val, int pktype) {
    robj *nbt          = btIndFindVal(ibtr, i_key, ibtr->ktype);
    bt   *nbtr         = (bt *)(nbt->ptr);
    ull   pre_size     = nbtr->malloc_size;
    int   n_size       = btIndNodeDelete(nbtr, i_val, pktype);
    ull   post_size    = nbtr->malloc_size;
    ibtr->malloc_size += (post_size - pre_size); /* inherits nbtr */
    if (!n_size) {
        btIndDelete(ibtr, i_key, ibtr->ktype);
        btRelease(nbtr, NULL);
    }
}

void addToIndex(redisDb *db, robj *pko, char *vals, uint32 cofsts[], int inum) {
    if (Index[server.dbid][inum].virt) return;
    bool  nrl     = Index[server.dbid][inum].nrl;
    robj *ind     = Index[server.dbid][inum].obj;
    robj *ibt     = lookupKey(db, ind);
    if (nrl) {
        nrlIndexAdd(ibt, pko, vals, cofsts);
        return;
    }
    bt   *ibtr    = (bt *)(ibt->ptr);
    int   i       = Index[server.dbid][inum].column;
    int   j       = i - 1;
    int   end     = cofsts[j];
    int   len     = cofsts[i] - end - 1;
    robj *col_key = createStringObject(vals + end, len); /* freeME */
    int   itm     = Index[server.dbid][inum].table;
    int   pktype  = Tbl[server.dbid][itm].col_type[0];

    iAdd(ibtr, col_key, pko, pktype);
    decrRefCount(col_key);
}

void delFromIndex(redisDb *db, robj *old_pk, robj *row, int inum, int tmatch) {
    if (Index[server.dbid][inum].virt) return;
    bool  nrl     = Index[server.dbid][inum].nrl;
    if (nrl) {
        RL4 "NRL delFromIndex");
        /* TODO add in nrldel */
        return;
    }
    robj *ind     = Index[server.dbid][inum].obj;
    int   cmatch  = Index[server.dbid][inum].column;
    robj *ibt     = lookupKey(db, ind);
    bt   *ibtr    = (bt *)(ibt->ptr);
    robj *old_val = createColObjFromRow(row, cmatch, old_pk, tmatch); /*freeME*/
    int   itm     = Index[server.dbid][inum].table;
    int   pktype  = Tbl[server.dbid][itm].col_type[0];

    iRem(ibtr, old_val, old_pk, pktype);
    decrRefCount(old_val);
}

void updateIndex(redisDb *db,
                 robj    *old_pk,
                 robj    *new_pk,
                 robj    *new_val,
                 robj    *row,
                 int      inum,
                 uchar    pk_update,
                 int      tmatch) {
    if (Index[server.dbid][inum].virt) return;
    bool  nrl     = Index[server.dbid][inum].nrl;
    if (nrl) {
        RL4 "NRL updateIndex");
        /* TODO add in nrldel */
        return;
    }
    int   cmatch  = Index[server.dbid][inum].column;
    robj *ind     = Index[server.dbid][inum].obj;
    robj *ibt     = lookupKey(db, ind);
    bt   *ibtr    = (bt *)(ibt->ptr);
    robj *old_val = createColObjFromRow(row, cmatch, old_pk, tmatch); //freeME
    int   itm     = Index[server.dbid][inum].table;
    int   pktype  = Tbl[server.dbid][itm].col_type[0];

    iRem(ibtr, old_val, old_pk, pktype);
    if (pk_update) iAdd(ibtr, old_val, new_pk, pktype);
    else           iAdd(ibtr, new_val, new_pk, pktype);
    decrRefCount(old_val);
}

// SIMPLE_COMMANDS SIMPLE_COMMANDS SIMPLE_COMMANDS SIMPLE_COMMANDS
// SIMPLE_COMMANDS SIMPLE_COMMANDS SIMPLE_COMMANDS SIMPLE_COMMANDS
void newIndex(redisClient *c,
              char        *iname,
              int          tmatch,
              int          cmatch,
              bool         virt,
              d_l_t       *nrlind) {
    // commit index definition
    robj *ind    = createStringObject(iname, strlen(iname));
    int   imatch = Num_indx[server.dbid];
    Index[server.dbid][imatch].obj     = ind;
    Index[server.dbid][imatch].table   = tmatch;
    Index[server.dbid][imatch].column  = cmatch;
    Index[server.dbid][imatch].type    = cmatch ?
        Tbl[server.dbid][tmatch].col_type[cmatch] : COL_TYPE_NONE;
    Index[server.dbid][imatch].virt    = virt;
    Index[server.dbid][imatch].nrl     = nrlind ? 1 : 0;

    robj *ibt;
    if (virt) {
        ibt                   = createEmptyBtreeObject();
        Tbl[server.dbid][tmatch].virt_indx = imatch;
    } else if (Index[server.dbid][imatch].nrl) {
        nrlind->num = imatch;
        ibt = createObject(REDIS_NRL_INDEX, nrlind);
    } else {
        int ctype = Tbl[server.dbid][tmatch].col_type[cmatch];
        ibt       = createBtreeObject(ctype, imatch, BTREE_INDEX);
    }
    //store BtreeObject in HashTable key: indexname
    dictAdd(c->db->dict, ind, ibt);
    Num_indx[server.dbid]++;
}

static bool parseNRLcmd(char *o_s,
                        list *nrltoks,
                        list *nrlcols,
                        int   tmatch) {
    char *s   = strchr(o_s, '$');
    if (!s) {
       listAddNodeTail(nrltoks, sdsdup(o_s)); /* freed in freeNrlIndexObject */
    } else {
        while (1) {
            s++; /* advance past "$" */
            char *nxo = s;
            while (isalnum(*nxo) || *nxo == '_') nxo++; /* col must be alpnum */
            char *nexts = strchr(s, '$');               /* var is '$' delimed */

            int cmatch = -1;
            if (nxo) cmatch = find_column_n(tmatch, s, nxo - s);
            else     cmatch = find_column(tmatch, s);
            if (cmatch == -1) return 0;
            listAddNodeTail(nrlcols, (void *)(long)(cmatch + 1)); /* 0!=NULL */

            listAddNodeTail(nrltoks, sdsnewlen(o_s, (s - 1) - o_s)); /*no "$"*/
            if (!nexts) { /* no more vars */
                if (*nxo) listAddNodeTail(nrltoks, sdsnewlen(nxo, strlen(nxo)));
                break;
            }
            o_s = nxo;
            s   = nexts;
        }
    }
    return 1;
}

sds rebuildOrigNRLcmd(robj *o) {
    d_l_t    *nrlind  = o->ptr;
    int       tmatch  = Index[server.dbid][nrlind->num].table;

    list     *nrltoks = nrlind->l1;
    list     *nrlcols = nrlind->l2;
    listIter *li1     = listGetIterator(nrltoks, AL_START_HEAD);
    listNode *ln1     = listNext(li1);
    listIter *li2     = listGetIterator(nrlcols, AL_START_HEAD);
    listNode *ln2     = listNext(li2);
    sds       cmd     = sdsnewlen("\"", 1); /* has to be one arg */
    while (ln1 || ln2) {
        if (ln1) { 
            sds token  = ln1->value;
            cmd        = sdscatlen(cmd, token, sdslen(token));
            ln1 = listNext(li1);
        }
        if (ln2) {
            int cmatch = (int)(long)ln2->value;
            cmatch--; /* because (0 != NULL) */
            sds cname  = Tbl[server.dbid][tmatch].col_name[cmatch]->ptr;
            cmd        = sdscatlen(cmd, "$", 1); /* "$" variable delim */
            cmd        = sdscatlen(cmd, cname, sdslen(cname));
            ln2 = listNext(li2);
        }
    }
    cmd = sdscatlen(cmd, "\"", 1); /* has to be one arg */
    return cmd;
}

static void indexCommit(redisClient *c,
                        char        *iname,
                        char        *trgt,
                        bool        nrl,
                        char       *nrltbl,
                        char       *nrladd,
                        char       *nrldel,
                        bool        build) {
    if (Num_indx[server.dbid] >= MAX_NUM_INDICES) {
        addReply(c, shared.toomanyindices);
        return;
    }

    if (match_index_name(iname) != -1) {
        addReply(c, shared.nonuniqueindexnames); 
        return;
    }

    sds    o_target = NULL; /* must come before first GOTO */
    d_l_t *nrlind   = NULL;
    int    cmatch   = - 1;
    int    tblmatch = - 1;

    if (!nrl) {
        // parse tablename.columnname
        o_target     = sdsdup(trgt);
        int   len    = sdslen(o_target);
        char *target = o_target;
        if (target[len - 1] == ')') target[len - 1] = '\0';
        if (*target         == '(') target++;
        char *column = strchr(target, CPERIOD);
        if (!column) {
            addReply(c, shared.indextargetinvalid);
            goto ind_commit_err;
        }
        *column = '\0';
        column++;
        TABLE_CHECK_OR_REPLY(target,)
        tblmatch = tmatch;
    
        cmatch = find_column(tmatch, column);
        if (cmatch == -1) {
            addReply(c, shared.indextargetinvalid);
            goto ind_commit_err;
        }
    
        for (int i = 0; i < Num_indx[server.dbid]; i++) { /* already indxd? */
            if (Index[server.dbid][i].table == tmatch &&
                Index[server.dbid][i].column == cmatch) {
                addReply(c, shared.indexedalready);
                goto ind_commit_err;
            }
        }
    } else {
        TABLE_CHECK_OR_REPLY(nrltbl,)
        tblmatch = tmatch;

        nrlind = malloc(sizeof(d_l_t)); /* freed in freeNrlIndexObject */
        nrlind->l1 = listCreate();
        nrlind->l2 = listCreate();
        if (!parseNRLcmd(nrladd, nrlind->l1, nrlind->l2, tmatch)) {
            addReply(c, shared.index_nonrel_decl_fmt);
            free(nrlind);
            goto ind_commit_err;
        }
    }

    newIndex(c, iname, tblmatch, cmatch, 0, nrlind);
    addReply(c, shared.ok);

    if (build) {
        /* IF table has rows - loop thru and populate index */
        robj *o   = lookupKeyRead(c->db, Tbl[server.dbid][tblmatch].name);
        bt   *btr = (bt *)o->ptr;
        if (btr->numkeys > 0) {
            robj *ind  = Index[server.dbid][Num_indx[server.dbid] - 1].obj;
            robj *ibt  = lookupKey(c->db, ind);
            buildIndex(btr, btr->root, ibt->ptr, cmatch, tblmatch, nrl);
        }
    }

ind_commit_err:
    if (o_target) sdsfree(o_target);
}

void createIndex(redisClient *c) {
    if (c->argc < 6) {
        addReply(c, shared.index_wrong_num_args);
        return;
    }

    char *nrldel = NULL;

    if (*((char *)c->argv[5]->ptr) != '(') {
        nrldel  = (c->argc > 6) ? c->argv[6]->ptr : NULL;
        indexCommit(c, c->argv[2]->ptr, NULL, 1,
                    c->argv[4]->ptr, c->argv[5]->ptr, nrldel, 1);
    } else {
        /* TODO lazy programming, change legacyIndex syntax */
        sds leg_col      = sdstrim(c->argv[5]->ptr, "()"); /* no free needed */
        sds leg_ind_sntx = sdscatprintf(sdsempty(), "%s.%s",
                                        (char *)c->argv[4]->ptr,
                                        (char *)leg_col);
        indexCommit(c, c->argv[2]->ptr, leg_ind_sntx, 0, NULL, NULL, NULL, 1);
        sdsfree(leg_ind_sntx);
    }
}

void legacyIndexCommand(redisClient *c) {
    bool nrl     = 0;
    char *trgt   = NULL;
    char *nrltbl = NULL;
    char *nrladd = NULL;
    char *nrldel = NULL;
    if (c->argc > 3) {
        nrl = 1;
        nrltbl = c->argv[2]->ptr;
        nrladd = (c->argc > 3) ? c->argv[3]->ptr : NULL;
        nrldel = (c->argc > 4) ? c->argv[4]->ptr : NULL;
    } else {
        trgt = c->argv[2]->ptr;
    }
    /* the final argument means -> if(nrl) dont build index */
    indexCommit(c, c->argv[1]->ptr, trgt, nrl, nrltbl, nrladd, nrldel, !nrl);
}


void emptyIndex(redisDb *db, int inum) {
    robj *ind                       = Index[server.dbid][inum].obj;
    deleteKey(db, ind);
    Index[server.dbid][inum].table  = -1;
    Index[server.dbid][inum].column = -1;
    Index[server.dbid][inum].type   = 0;
    Index[server.dbid][inum].virt   = 0;
    Index[server.dbid][inum].obj    = NULL;
    server.dirty++;
    //TODO shuffle indices to make space for deleted indices
}

void dropIndex(redisClient *c) {
    char *iname  = c->argv[2]->ptr;
    int   inum   = match_index_name(iname);

    if (Index[server.dbid][inum].virt) {
        addReply(c, shared.drop_virtual_index);
        return;
    }

    emptyIndex(c->db, inum);
    addReply(c, shared.cone);
}


/* RANGE_OPS RANGE_OPS RANGE_OPS RANGE_OPS RANGE_OPS RANGE_OPS RANGE_OPS */
/* RANGE_OPS RANGE_OPS RANGE_OPS RANGE_OPS RANGE_OPS RANGE_OPS RANGE_OPS */

/* TODO cleanup this fugly workaround */
bool range_check_or_reply(redisClient *c, char *r, robj **l, robj **h) {
    RANGE_CHECK_OR_REPLY(r, 0)
    *l = low;
    *h = high;
    return 1;
}

#define ISELECT_OPERATION(Q)                                    \
    robj *r = outputRow(row, qcols, cmatchs, key, tmatch, 0);   \
    if (Q) addORowToRQList(ll, r, row, obc, key, tmatch, icol); \
    else   addReplyBulk(c, r);                                  \
    decrRefCount(r);

void iselectAction(redisClient *c,
                   robj        *rng,
                   int          tmatch,
                   int          imatch,
                   char        *col_list,
                   int          obc,
                   bool         asc,
                   int          lim,
                   list        *inl) {
    int cmatchs[MAX_COLUMN_PER_TABLE];
    int   qcols = parseColListOrReply(c, tmatch, col_list, cmatchs);
    if (!qcols) {
        addReply(c, shared.nullbulk);
        return;
    }

    char *range = rng ? rng->ptr : NULL;
    robj *low   = NULL;
    robj *high  = NULL;
    if (range && !range_check_or_reply(c, range, &low, &high)) return;

    list *ll   = NULL;
    bool  icol = 0;
    if (obc != -1) {
        ll = listCreate();
        icol = (Tbl[server.dbid][tmatch].col_type[obc] == COL_TYPE_INT);
    }

    bool              qed = 0;
    btStreamIterator *bi  = NULL;
    btStreamIterator *nbi = NULL;
    LEN_OBJ
    if (range) { /* RANGE QUERY */
        RANGE_QUERY_LOOKUP_START
            ISELECT_OPERATION(q_pk)
        RANGE_QUERY_LOOKUP_MIDDLE
                ISELECT_OPERATION(q_fk)
        RANGE_QUERY_LOOKUP_END
    } else {    /* IN () QUERY */
        IN_QUERY_LOOKUP_START
            ISELECT_OPERATION(q_pk)
        IN_QUERY_LOOKUP_MIDDLE
                ISELECT_OPERATION(q_fk)
        IN_QUERY_LOOKUP_END
    }

    if (qed && card) {
        obsl_t **vector = sortOrderByToVector(ll, icol, asc);
        for (int k = 0; k < (int)listLength(ll); k++) {
            if (lim != -1 && k == lim) break;
            obsl_t *ob = vector[k];
            addReplyBulk(c, ob->row);
        }
        sortedOrderByCleanup(vector, listLength(ll), icol, 1);
        free(vector);
    }
    if (ll)   listRelease(ll);
    if (low)  decrRefCount(low);
    if (high) decrRefCount(high);

    if (lim != -1 && (uint32)lim < card) card = lim;
    lenobj->ptr = sdscatprintf(sdsempty(), "*%lu\r\n", card);
}

static void addPKtoRQList(list *ll,
                          robj *pko,
                          robj *row,
                          int   obc,
                          int   tmatch,
                          bool  icol) {
    addORowToRQList(ll, pko, row, obc, pko, tmatch, icol);
}

#define BUILD_RQ_OPERATION(Q)                                    \
    if (Q) {                                                     \
        addPKtoRQList(ll, key, row, obc, tmatch, icol);          \
    } else {                                                     \
        robj *cln  = cloneRobj(key); /* clone orig is BtRobj */  \
        listAddNodeTail(ll, cln);                                \
    }


#define BUILD_RANGE_QUERY_LIST                                                \
    char *range = rng ? rng->ptr : NULL;                                      \
    robj *low   = NULL;                                                       \
    robj *high  = NULL;                                                       \
    if (range && !range_check_or_reply(c, range, &low, &high)) return;        \
                                                                              \
    list *ll   = listCreate();                                                \
    bool  icol = 0;                                                           \
    if (obc != -1) {                                                          \
        icol = (Tbl[server.dbid][tmatch].col_type[obc] == COL_TYPE_INT);      \
    }                                                                         \
                                                                              \
    bool              qed  = 0;                                               \
    ulong             card = 0;                                               \
    btStreamIterator *bi   = NULL;                                            \
    btStreamIterator *nbi  = NULL;                                            \
    if (range) { /* RANGE QUERY */                                            \
        RANGE_QUERY_LOOKUP_START                                              \
            BUILD_RQ_OPERATION(q_pk)                                          \
        RANGE_QUERY_LOOKUP_MIDDLE                                             \
                BUILD_RQ_OPERATION(q_fk)                                      \
        RANGE_QUERY_LOOKUP_END                                                \
    } else {    /* IN () QUERY */                                             \
        IN_QUERY_LOOKUP_START                                                 \
            BUILD_RQ_OPERATION(q_pk)                                          \
        IN_QUERY_LOOKUP_MIDDLE                                                \
                BUILD_RQ_OPERATION(q_fk)                                      \
        IN_QUERY_LOOKUP_END                                                   \
    }

void ideleteAction(redisClient *c,
                   robj        *rng,
                   int          tmatch,
                   int          imatch,
                   int          obc,
                   bool         asc,
                   int          lim,
                   list        *inl) {
    BUILD_RANGE_QUERY_LIST

    MATCH_INDICES(tmatch)

    if (card) {
        if (qed) {
            obsl_t **vector = sortOrderByToVector(ll, icol, asc);
            for (int k = 0; k < (int)listLength(ll); k++) {
                if (lim != -1 && k == lim) break;
                obsl_t *ob = vector[k];
                robj *nkey = ob->row;
                deleteRow(c, tmatch, nkey, matches, indices);
            }
            sortedOrderByCleanup(vector, listLength(ll), icol, 1);
            free(vector);
        } else {
            listNode  *ln;
            listIter  *li = listGetIterator(ll, AL_START_HEAD);
            while((ln = listNext(li)) != NULL) {
                robj *nkey = ln->value;
                deleteRow(c, tmatch, nkey, matches, indices);
                decrRefCount(nkey); /* from cloneRobj in BUILD_RQ_OPERATION */
            }
            listReleaseIterator(li);
        }
    }

    if (lim != -1 && (uint32)lim < card) card = lim;
    addReplyLongLong(c, card);

    listRelease(ll);
    if (low)  decrRefCount(low);
    if (high) decrRefCount(high);
}

void iupdateAction(redisClient *c,
                   robj        *rng,
                   int          tmatch,
                   int          imatch,
                   int          ncols,
                   int          matches,
                   int          indices[],
                   char        *vals[],
                   uint32       vlens[],
                   uchar        cmiss[],
                   int          obc,
                   bool         asc,
                   int          lim,
                   list        *inl) {

    BUILD_RANGE_QUERY_LIST

    if (card) {
        robj *o        = lookupKeyRead(c->db, Tbl[server.dbid][tmatch].name);
        bool  pktype   = Tbl[server.dbid][tmatch].col_type[0];
        if (qed) {
            obsl_t **vector = sortOrderByToVector(ll, icol, asc);
            for (int k = 0; k < (int)listLength(ll); k++) {
                if (lim != -1 && k == lim) break;
                obsl_t *ob = vector[k];
                robj *nkey = ob->row;
                robj *row  = btFindVal(o, nkey, pktype);
                updateRow(c, o, nkey, row,
                          tmatch, ncols, matches, indices, vals, vlens, cmiss);
            }
            sortedOrderByCleanup(vector, listLength(ll), icol, 1);
            free(vector);
        } else {
            listNode  *ln;
            listIter  *li = listGetIterator(ll, AL_START_HEAD);
            while((ln = listNext(li)) != NULL) {
                robj *nkey = ln->value;
                robj *row  = btFindVal(o, nkey, pktype);
                updateRow(c, o, nkey, row,
                          tmatch, ncols, matches, indices, vals, vlens, cmiss);
                decrRefCount(nkey); /* from cloneRobj in BUILD_RQ_OPERATION */
            }
            listReleaseIterator(li);
        }
    }

    if (lim != -1 && (uint32)lim < card) card = lim;
    addReplyLongLong(c, card);

    listRelease(ll);
    if (low)  decrRefCount(low);
    if (high) decrRefCount(high);
}

void ikeysCommand(redisClient *c) {
    int   imatch = checkIndexedColumnOrReply(c, c->argv[1]->ptr);
    if (imatch == -1) return;
    RANGE_CHECK_OR_REPLY(c->argv[2]->ptr,)
    LEN_OBJ

    btEntry    *be, *nbe;
    robj       *ind  = Index[server.dbid][imatch].obj;
    bool        virt = Index[server.dbid][imatch].virt;
    robj       *bt   = lookupKey(c->db, ind);
    btStreamIterator *bi   = btGetRangeIterator(bt, low, high, virt);
    while ((be = btRangeNext(bi, 1)) != NULL) {                // iterate btree
        if (virt) {
            robj *pko = be->key;
            addReplyBulk(c, pko);
            card++;
        } else {
            robj       *val = be->val;
            btStreamIterator *nbi = btGetFullRangeIterator(val, 0, 0);
            while ((nbe = btRangeNext(nbi, 1)) != NULL) {     // iterate NodeBT
                robj *nkey = nbe->key;
                addReplyBulk(c, nkey);
                card++;
            }
            btReleaseRangeIterator(nbi);
        }
    }
    btReleaseRangeIterator(bi);
    decrRefCount(low);
    decrRefCount(high);

    lenobj->ptr = sdscatprintf(sdsempty(), "*%lu\r\n", card);
}

#define ADD_REPLY_BULK(r, buf)                \
    r = createStringObject(buf, strlen(buf)); \
    addReplyBulk(c, r);                       \
    decrRefCount(r);                          \
    card++;

void dumpCommand(redisClient *c) {
    char buf[192];
    TABLE_CHECK_OR_REPLY(c->argv[1]->ptr,)
    robj *o = lookupKeyRead(c->db, Tbl[server.dbid][tmatch].name);

    bt *btr = (bt *)o->ptr;
    int   cmatchs[MAX_COLUMN_PER_TABLE];
    int   qcols = parseColListOrReply(c, tmatch, "*", cmatchs);
    char *tname = Tbl[server.dbid][tmatch].name->ptr;

    LEN_OBJ

    bool  to_mysql = 0;
    bool  ret_size = 0;
    char *m_tname  = tname;
    if (c->argc > 3) {
        if (!strcasecmp(c->argv[2]->ptr, "TO") &&
            !strcasecmp(c->argv[3]->ptr, "MYSQL")      ) {
            to_mysql = 1;
            if (c->argc > 4) m_tname = c->argv[4]->ptr;
            robj *r;
            sprintf(buf, "DROP TABLE IF EXISTS `%s`;", m_tname);
            ADD_REPLY_BULK(r, buf)
            sprintf(buf, "CREATE TABLE `%s` ( ", m_tname);
            r = createStringObject(buf, strlen(buf));
            for (int i = 0; i < Tbl[server.dbid][tmatch].col_count; i++) {
                bool is_int =
                         (Tbl[server.dbid][tmatch].col_type[i] == COL_TYPE_INT);
                r->ptr = sdscatprintf(r->ptr, "%s %s %s%s",
                          (i == 0) ? ""        : ",",
                          (char *)Tbl[server.dbid][tmatch].col_name[i]->ptr,
                          is_int ? "INT" : (i == 0) ? "VARCHAR(512)" : "TEXT",
                          (i == 0) ? " PRIMARY KEY" : "");
            }
            r->ptr = sdscat(r->ptr, ");");
            addReplyBulk(c, r);
            decrRefCount(r);
            card++;
            sprintf(buf, "LOCK TABLES `%s` WRITE;", m_tname);
            ADD_REPLY_BULK(r, buf)
        } else if (!strcasecmp(c->argv[2]->ptr, "RETURN") &&
                   !strcasecmp(c->argv[3]->ptr, "SIZE")      ) {
            ret_size = 1;
            sprintf(buf, "KEYS: %d BT-DATA: %lld BT-MALLOC: %lld",
                          btr->numkeys, btr->data_size, btr->malloc_size);
            robj *r = createStringObject(buf, strlen(buf));
            addReplyBulk(c, r);
            decrRefCount(r);
            card++;
        }
    }

    if (btr->numkeys) {
        btEntry    *be;
        btStreamIterator *bi = btGetFullRangeIterator(o, 0, 1);
        while ((be = btRangeNext(bi, 0)) != NULL) {      // iterate btree
            robj *pko = be->key;
            robj *row = be->val;
            robj *r   = outputRow(row, qcols, cmatchs, pko, tmatch, to_mysql);
            if (!to_mysql) {
                addReplyBulk(c, r);
                decrRefCount(r);
            } else {
                sprintf(buf, "INSERT INTO `%s` VALUES (", m_tname);
                robj *ins = createStringObject(buf, strlen(buf));
                ins->ptr  = sdscatlen(ins->ptr, r->ptr, sdslen(r->ptr));
                ins->ptr  = sdscatlen(ins->ptr, ");", 2);
                addReplyBulk(c, ins);
                decrRefCount(ins);
            }
            card++;
        }
        btReleaseRangeIterator(bi);
    }

    if (to_mysql) {
        robj *r;
        sprintf(buf, "UNLOCK TABLES;");
        ADD_REPLY_BULK(r, buf)
    }
    lenobj->ptr = sdscatprintf(sdsempty(), "*%lu\r\n", card);
}
 
ull get_sum_all_index_size_for_table(redisClient *c, int tmatch) {
    ull isize = 0;
    for (int i = 0; i < Num_indx[server.dbid]; i++) {
        if (!Index[server.dbid][i].virt && Index[server.dbid][i].table == tmatch) {
            robj *ind   = Index[server.dbid][i].obj;
            robj *ibt   = lookupKey(c->db, ind);
            bt   *ibtr  = (bt *)(ibt->ptr);
            isize      += ibtr->malloc_size;
        }
    }
    return isize;
}

static void zero(robj *r) {
    r->encoding = REDIS_ENCODING_RAW;
    r->ptr      = 0;
}

void descCommand(redisClient *c) {
    char buf[256];
    TABLE_CHECK_OR_REPLY( c->argv[1]->ptr,)
    robj *o = lookupKeyRead(c->db, Tbl[server.dbid][tmatch].name);

    LEN_OBJ;
    for (int j = 0; j < Tbl[server.dbid][tmatch].col_count; j++) {
        robj *r      = createObject(REDIS_STRING, NULL);
        int   imatch = find_index(tmatch, j);
        if (imatch == -1) {
            r->ptr  = sdscatprintf(sdsempty(), "%s | %s ",
                           (char *)Tbl[server.dbid][tmatch].col_name[j]->ptr,
                           Col_type_defs[Tbl[server.dbid][tmatch].col_type[j]]);
        } else {
            robj *ind    = Index[server.dbid][imatch].obj;
            ull   isize  = 0;
            if (!Index[server.dbid][imatch].virt) {
                robj *ibt  = lookupKey(c->db, ind);
                bt   *ibtr = (bt *)(ibt->ptr);
                isize      = ibtr ? ibtr->malloc_size : 0;
            }
            r->ptr = sdscatprintf(sdsempty(),
                            "%s | %s | INDEX: %s [BYTES: %lld]",
                            (char *)Tbl[server.dbid][tmatch].col_name[j]->ptr,
                            Col_type_defs[Tbl[server.dbid][tmatch].col_type[j]],
                            (char *)ind->ptr, isize);
        }
        addReplyBulk(c, r);
        decrRefCount(r);
	card++;
    }
    ull  index_size = get_sum_all_index_size_for_table(c, tmatch);
    bt  *btr        = (bt *)o->ptr;
    robj minkey, maxkey;
    if (!btr->numkeys || !assignMinKey(btr, &minkey)) zero(&minkey);
    if (!btr->numkeys || !assignMaxKey(btr, &maxkey)) zero(&maxkey);

    if (minkey.encoding == REDIS_ENCODING_RAW) {
        if (minkey.ptr && sdslen(minkey.ptr) > 64) {
            char *x = (char *)(minkey.ptr);
            x[64] ='\0';
        }
        if (maxkey.ptr && sdslen(maxkey.ptr) > 64) {
            char *x = (char *)(maxkey.ptr);
            x[64] ='\0';
        }
        sprintf(buf, "INFO: KEYS: [NUM: %d MIN: %s MAX: %s]"\
                          " BYTES: [BT-DATA: %lld BT-TOTAL: %lld INDEX: %lld]",
                btr->numkeys, (char *)minkey.ptr, (char *)maxkey.ptr,
                btr->data_size, btr->malloc_size, index_size);
    } else {
        sprintf(buf, "INFO: KEYS: [NUM: %d MIN: %u MAX: %u]"\
                          " BYTES: [BT-DATA: %lld BT-TOTAL: %lld INDEX: %lld]",
            btr->numkeys, (uint32)(long)minkey.ptr, (uint32)(long)maxkey.ptr,
            btr->data_size, btr->malloc_size, index_size);
    }
    robj *r = createStringObject(buf, strlen(buf));
    addReplyBulk(c, r);
    decrRefCount(r);
    card++;
    lenobj->ptr = sdscatprintf(sdsempty(), "*%lu\r\n", card);
}

void freeNrlIndexObject(robj *o) {
    listNode *ln;
    d_l_t    *nrlind = (d_l_t *)o->ptr;
    listIter *li     = listGetIterator(nrlind->l1, AL_START_HEAD);
    while((ln = listNext(li)) != NULL) {
        sdsfree(ln->value); /* free sds* from parseNRLcmd() */
    }
    listRelease(nrlind->l1);
    listRelease(nrlind->l2);
    free(nrlind);
}

#if 0
/* LEGACY CODE - the ROOTS */
void iselectCommand(redisClient *c) {
    int imatch = checkIndexedColumnOrReply(c, c->argv[1]->ptr);
    if (imatch == -1) return;
    int tmatch = Index[server.dbid][imatch].table;

    iselectAction(c, c->argv[2], tmatch, imatch, c->argv[3]->ptr);
}

void iupdateCommand(redisClient *c) {
    int   imatch = checkIndexedColumnOrReply(c, c->argv[1]->ptr);
    if (imatch == -1) return;
    int   tmatch = Index[server.dbid][imatch].table;
    int   ncols   = Tbl[server.dbid][tmatch]._col_count;

    int   cmatchs  [MAX_COLUMN_PER_TABLE];
    char *mvals    [MAX_COLUMN_PER_TABLE];
    int   mvlens   [MAX_COLUMN_PER_TABLE];
    int   qcols = parseUpdateOrReply(c, tmatch, c->argv[3]->ptr, cmatchs,
                                     mvals, mvlens);
    if (!qcols) return;

    MATCH_INDICES(tmatch)
    ASSIGN_UPDATE_HITS_AND_MISSES

    iupdateAction(c, c->argv[2]->ptr, tmatch, imatch, ncols, matches, indices,
                  vals, vlens, cmiss);
}

void ideleteCommand(redisClient *c) {
    int   imatch = checkIndexedColumnOrReply(c, c->argv[1]->ptr);
    if (imatch == -1) return;
    int   tmatch = Index[server.dbid][imatch].table;
    ideleteAction(c, c->argv[2]->ptr, tmatch, imatch);
}
#endif
