/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains C code routines that are called by the parser
** to handle UPDATE statements.
**
** $Id: update.c,v 1.62 2003/04/22 20:30:40 drh Exp $
*/
#include "sqliteInt.h"

/*
** Process an UPDATE statement.
**
**   UPDATE OR IGNORE table_wxyz SET a=b, c=d WHERE e<5 AND f NOT NULL;
**          \_______/ \________/     \______/       \________________/
*            onError   pTabList      pChanges             pWhere
*/
void sqliteUpdate(
  Parse *pParse,         /* The parser context */
  SrcList *pTabList,     /* The table in which we should change things */
  ExprList *pChanges,    /* Things to be changed */
  Expr *pWhere,          /* The WHERE clause.  May be null */
  int onError            /* How to handle constraint errors */
){
  int i, j;              /* Loop counters */
  Table *pTab;           /* The table to be updated */
  int addr;              /* VDBE instruction address of the start of the loop */
  WhereInfo *pWInfo;     /* Information about the WHERE clause */
  Vdbe *v;               /* The virtual database engine */
  Index *pIdx;           /* For looping over indices */
  int nIdx;              /* Number of indices that need updating */
  int nIdxTotal;         /* Total number of indices */
  int base;              /* Index of first available table cursor */
  sqlite *db;            /* The database structure */
  Index **apIdx = 0;     /* An array of indices that need updating too */
  char *aIdxUsed = 0;    /* aIdxUsed[i]==1 if the i-th index is used */
  int *aXRef = 0;        /* aXRef[i] is the index in pChanges->a[] of the
                         ** an expression for the i-th column of the table.
                         ** aXRef[i]==-1 if the i-th column is not changed. */
  int chngRecno;         /* True if the record number is being changed */
  Expr *pRecnoExpr;      /* Expression defining the new record number */
  int openAll;           /* True if all indices need to be opened */

  int before_triggers;         /* True if there are any BEFORE triggers */
  int after_triggers;          /* True if there are any AFTER triggers */
  int row_triggers_exist = 0;  /* True if any row triggers exist */

  int newIdx      = -1;  /* index of trigger "new" temp table       */
  int oldIdx      = -1;  /* index of trigger "old" temp table       */

  if( pParse->nErr || sqlite_malloc_failed ) goto update_cleanup;
  db = pParse->db;
  assert( pTabList->nSrc==1 );

  /* Locate the table which we want to update. 
  */
  pTab = sqliteSrcListLookup(pParse, pTabList);
  if( pTab==0 ) goto update_cleanup;
  before_triggers = sqliteTriggersExist(pParse, pTab->pTrigger, 
            TK_UPDATE, TK_BEFORE, TK_ROW, pChanges);
  after_triggers = sqliteTriggersExist(pParse, pTab->pTrigger, 
            TK_UPDATE, TK_AFTER, TK_ROW, pChanges);
  row_triggers_exist = before_triggers || after_triggers;
  if( row_triggers_exist &&  pTab->pSelect ){
    /* Just fire VIEW triggers */
    sqliteSrcListDelete(pTabList);
    sqliteViewTriggers(pParse, pTab, pWhere, onError, pChanges);
    return;
  }
  if( sqliteIsReadOnly(pParse, pTab) ) goto update_cleanup;
  assert( pTab->pSelect==0 );  /* This table is not a VIEW */
  aXRef = sqliteMalloc( sizeof(int) * pTab->nCol );
  if( aXRef==0 ) goto update_cleanup;
  for(i=0; i<pTab->nCol; i++) aXRef[i] = -1;

  /* If there are FOR EACH ROW triggers, allocate cursors for the
  ** special OLD and NEW tables
  */
  if( row_triggers_exist ){
    newIdx = pParse->nTab++;
    oldIdx = pParse->nTab++;
  }

  /* Allocate a cursors for the main database table and for all indices.
  ** The index cursors might not be used, but if they are used they
  ** need to occur right after the database cursor.  So go ahead and
  ** allocate enough space, just in case.
  */
  base = pParse->nTab++;
  for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
    pParse->nTab++;
  }

  /* Resolve the column names in all the expressions in both the
  ** WHERE clause and in the new values.  Also find the column index
  ** for each column to be updated in the pChanges array.  For each
  ** column to be updated, make sure we have authorization to change
  ** that column.
  */
  if( pWhere ){
    if( sqliteExprResolveIds(pParse, base, pTabList, 0, pWhere) ){
      goto update_cleanup;
    }
    if( sqliteExprCheck(pParse, pWhere, 0, 0) ){
      goto update_cleanup;
    }
  }
  chngRecno = 0;
  for(i=0; i<pChanges->nExpr; i++){
    if( sqliteExprResolveIds(pParse, base, pTabList, 0, pChanges->a[i].pExpr) ){
      goto update_cleanup;
    }
    if( sqliteExprCheck(pParse, pChanges->a[i].pExpr, 0, 0) ){
      goto update_cleanup;
    }
    for(j=0; j<pTab->nCol; j++){
      if( sqliteStrICmp(pTab->aCol[j].zName, pChanges->a[i].zName)==0 ){
        if( j==pTab->iPKey ){
          chngRecno = 1;
          pRecnoExpr = pChanges->a[i].pExpr;
        }
        aXRef[j] = i;
        break;
      }
    }
    if( j>=pTab->nCol ){
      sqliteErrorMsg(pParse, "no such column: %s", pChanges->a[i].zName);
      goto update_cleanup;
    }
#ifndef SQLITE_OMIT_AUTHORIZATION
    {
      int rc;
      rc = sqliteAuthCheck(pParse, SQLITE_UPDATE, pTab->zName,
                           pTab->aCol[j].zName, db->aDb[pTab->iDb].zName);
      if( rc==SQLITE_DENY ){
        goto update_cleanup;
      }else if( rc==SQLITE_IGNORE ){
        aXRef[j] = -1;
      }
    }
#endif
  }

  /* Allocate memory for the array apIdx[] and fill it with pointers to every
  ** index that needs to be updated.  Indices only need updating if their
  ** key includes one of the columns named in pChanges or if the record
  ** number of the original table entry is changing.
  */
  for(nIdx=nIdxTotal=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, nIdxTotal++){
    if( chngRecno ){
      i = 0;
    }else {
      for(i=0; i<pIdx->nColumn; i++){
        if( aXRef[pIdx->aiColumn[i]]>=0 ) break;
      }
    }
    if( i<pIdx->nColumn ) nIdx++;
  }
  if( nIdxTotal>0 ){
    apIdx = sqliteMalloc( sizeof(Index*) * nIdx + nIdxTotal );
    if( apIdx==0 ) goto update_cleanup;
    aIdxUsed = (char*)&apIdx[nIdx];
  }
  for(nIdx=j=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, j++){
    if( chngRecno ){
      i = 0;
    }else{
      for(i=0; i<pIdx->nColumn; i++){
        if( aXRef[pIdx->aiColumn[i]]>=0 ) break;
      }
    }
    if( i<pIdx->nColumn ){
      apIdx[nIdx++] = pIdx;
      aIdxUsed[j] = 1;
    }else{
      aIdxUsed[j] = 0;
    }
  }

  /* Begin generating code.
  */
  v = sqliteGetVdbe(pParse);
  if( v==0 ) goto update_cleanup;
  sqliteBeginWriteOperation(pParse, 1, !row_triggers_exist && pTab->iDb==1);

  /* Begin the database scan
  */
  pWInfo = sqliteWhereBegin(pParse, base, pTabList, pWhere, 1, 0);
  if( pWInfo==0 ) goto update_cleanup;

  /* Remember the index of every item to be updated.
  */
  sqliteVdbeAddOp(v, OP_ListWrite, 0, 0);

  /* End the database scan loop.
  */
  sqliteWhereEnd(pWInfo);

  /* Initialize the count of updated rows
  */
  if( db->flags & SQLITE_CountRows && !pParse->trigStack ){
    sqliteVdbeAddOp(v, OP_Integer, 0, 0);
  }

  if( row_triggers_exist ){
    /* Create pseudo-tables for NEW and OLD
    */
    sqliteVdbeAddOp(v, OP_OpenPseudo, oldIdx, 0);
    sqliteVdbeAddOp(v, OP_OpenPseudo, newIdx, 0);

    /* The top of the update loop for when there are triggers.
    */
    sqliteVdbeAddOp(v, OP_ListRewind, 0, 0);
    addr = sqliteVdbeAddOp(v, OP_ListRead, 0, 0);
    sqliteVdbeAddOp(v, OP_Dup, 0, 0);

    /* Open a cursor and make it point to the record that is
    ** being updated.
    */
    sqliteVdbeAddOp(v, OP_Dup, 0, 0);
    sqliteVdbeAddOp(v, OP_Integer, pTab->iDb, 0);
    sqliteVdbeAddOp(v, OP_OpenRead, base, pTab->tnum);
    sqliteVdbeAddOp(v, OP_MoveTo, base, 0);

    /* Generate the OLD table
    */
    sqliteVdbeAddOp(v, OP_Recno, base, 0);
    sqliteVdbeAddOp(v, OP_RowData, base, 0);
    sqliteVdbeAddOp(v, OP_PutIntKey, oldIdx, 0);

    /* Generate the NEW table
    */
    if( chngRecno ){
      sqliteExprCode(pParse, pRecnoExpr);
    }else{
      sqliteVdbeAddOp(v, OP_Recno, base, 0);
    }
    for(i=0; i<pTab->nCol; i++){
      if( i==pTab->iPKey ){
        sqliteVdbeAddOp(v, OP_String, 0, 0);
        continue;
      }
      j = aXRef[i];
      if( j<0 ){
        sqliteVdbeAddOp(v, OP_Column, base, i);
      }else{
        sqliteExprCode(pParse, pChanges->a[j].pExpr);
      }
    }
    sqliteVdbeAddOp(v, OP_MakeRecord, pTab->nCol, 0);
    sqliteVdbeAddOp(v, OP_PutIntKey, newIdx, 0);
    sqliteVdbeAddOp(v, OP_Close, base, 0);

    /* Fire the BEFORE triggers
    */
    if( sqliteCodeRowTrigger(pParse, TK_UPDATE, pChanges, TK_BEFORE, pTab, 
          newIdx, oldIdx, onError, addr) ){
      goto update_cleanup;
    }
  }

  /* Rewind the list of records that need to be updated and
  ** open every index that needs updating.  Note that if any
  ** index could potentially invoke a REPLACE conflict resolution 
  ** action, then we need to open all indices because we might need
  ** to be deleting some records.
  */
  sqliteVdbeAddOp(v, OP_Integer, pTab->iDb, 0);
  sqliteVdbeAddOp(v, OP_OpenWrite, base, pTab->tnum);
  if( onError==OE_Replace ){
    openAll = 1;
  }else{
    openAll = 0;
    for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
      if( pIdx->onError==OE_Replace ){
        openAll = 1;
        break;
      }
    }
  }
  for(i=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, i++){
    if( openAll || aIdxUsed[i] ){
      sqliteVdbeAddOp(v, OP_Integer, pIdx->iDb, 0);
      sqliteVdbeAddOp(v, OP_OpenWrite, base+i+1, pIdx->tnum);
      assert( pParse->nTab>base+i+1 );
    }
  }

  /* Loop over every record that needs updating.  We have to load
  ** the old data for each record to be updated because some columns
  ** might not change and we will need to copy the old value.
  ** Also, the old data is needed to delete the old index entires.
  ** So make the cursor point at the old record.
  */
  if( !row_triggers_exist ){
    sqliteVdbeAddOp(v, OP_ListRewind, 0, 0);
    addr = sqliteVdbeAddOp(v, OP_ListRead, 0, 0);
    sqliteVdbeAddOp(v, OP_Dup, 0, 0);
  }
  sqliteVdbeAddOp(v, OP_NotExists, base, addr);

  /* If the record number will change, push the record number as it
  ** will be after the update. (The old record number is currently
  ** on top of the stack.)
  */
  if( chngRecno ){
    sqliteExprCode(pParse, pRecnoExpr);
    sqliteVdbeAddOp(v, OP_MustBeInt, 0, 0);
  }

  /* Compute new data for this record.  
  */
  for(i=0; i<pTab->nCol; i++){
    if( i==pTab->iPKey ){
      sqliteVdbeAddOp(v, OP_String, 0, 0);
      continue;
    }
    j = aXRef[i];
    if( j<0 ){
      sqliteVdbeAddOp(v, OP_Column, base, i);
    }else{
      sqliteExprCode(pParse, pChanges->a[j].pExpr);
    }
  }

  /* Do constraint checks
  */
  sqliteGenerateConstraintChecks(pParse, pTab, base, aIdxUsed, chngRecno, 1,
                                 onError, addr);

  /* Delete the old indices for the current record.
  */
  sqliteGenerateRowIndexDelete(db, v, pTab, base, aIdxUsed);

  /* If changing the record number, delete the old record.
  */
  if( chngRecno ){
    sqliteVdbeAddOp(v, OP_Delete, base, 0);
  }

  /* Create the new index entries and the new record.
  */
  sqliteCompleteInsertion(pParse, pTab, base, aIdxUsed, chngRecno, 1, -1);

  /* Increment the row counter 
  */
  if( db->flags & SQLITE_CountRows && !pParse->trigStack){
    sqliteVdbeAddOp(v, OP_AddImm, 1, 0);
  }

  if( row_triggers_exist ){
    for(i=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, i++){
      if( openAll || aIdxUsed[i] )
        sqliteVdbeAddOp(v, OP_Close, base+i+1, 0);
    }
    sqliteVdbeAddOp(v, OP_Close, base, 0);
    pParse->nTab = base;

    if( sqliteCodeRowTrigger(pParse, TK_UPDATE, pChanges, TK_AFTER, pTab, 
          newIdx, oldIdx, onError, addr) ){
      goto update_cleanup;
    }
  }

  /* Repeat the above with the next record to be updated, until
  ** all record selected by the WHERE clause have been updated.
  */
  sqliteVdbeAddOp(v, OP_Goto, 0, addr);
  sqliteVdbeChangeP2(v, addr, sqliteVdbeCurrentAddr(v));
  sqliteVdbeAddOp(v, OP_ListReset, 0, 0);

  /* Close all tables if there were no FOR EACH ROW triggers */
  if( !row_triggers_exist ){
    for(i=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, i++){
      if( openAll || aIdxUsed[i] ){
        sqliteVdbeAddOp(v, OP_Close, base+i+1, 0);
      }
    }
    sqliteVdbeAddOp(v, OP_Close, base, 0);
    pParse->nTab = base;
  }else{
    sqliteVdbeAddOp(v, OP_Close, newIdx, 0);
    sqliteVdbeAddOp(v, OP_Close, oldIdx, 0);
  }

  sqliteEndWriteOperation(pParse);

  /*
  ** Return the number of rows that were changed.
  */
  if( db->flags & SQLITE_CountRows && !pParse->trigStack ){
    sqliteVdbeAddOp(v, OP_ColumnName, 0, 0);
    sqliteVdbeChangeP3(v, -1, "rows updated", P3_STATIC);
    sqliteVdbeAddOp(v, OP_Callback, 1, 0);
  }

update_cleanup:
  sqliteFree(apIdx);
  sqliteFree(aXRef);
  sqliteSrcListDelete(pTabList);
  sqliteExprListDelete(pChanges);
  sqliteExprDelete(pWhere);
  return;
}
