/* cm_modelconfig.c
 * SRE, Wed May  8 14:30:38 2002 [St. Louis]
 * SVN $Id$
 * 
 * Configuring a model into different global or local modes.
 * 
 ******************************************************************
 * @LICENSE@
 ******************************************************************
 */

#include "esl_config.h"
#include "config.h"

#include <string.h>

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_stack.h"
#include "esl_vectorops.h"

#include "funcs.h"
#include "structs.h"

/*
 * Function: ConfigCM()
 * Date:     EPN, Thu Jan  4 06:36:09 2007
 * Purpose:  Configure a CM for alignment or search based on cm->config_opts,
 *           cm->align_opts and cm->search_opts. 
 *           Always builds CP9 HMM (it's fast).
 *           Calculates query dependent bands (QDBs) if nec.
 *           QDBs can also be passed in. 
 * 
 * Args:     CM           - the covariance model
 *           preset_dmin  - supplied dmin values, NULL if none
 *           preset_dmax  - supplied dmax values, NULL if none
 *
 * Returns:   <eslOK> on success.
 *            <eslEINVAL> on contract violation.
 *            <eslEMEM> on memory allocation error.
 */
int 
ConfigCM(CM_t *cm, int *preset_dmin, int *preset_dmax)
{
  int status;
  float swentry, swexit;
  int do_calc_qdb   = FALSE;
  int do_preset_qdb = FALSE;
  int v;
  
  /* Check if we need to calculate QDBs and/or build a CP9 HMM. */
  if(cm->config_opts & CM_CONFIG_QDB) {
    if(preset_dmin == NULL && preset_dmax == NULL) 
      do_calc_qdb   = TRUE;
    else 
      do_preset_qdb = TRUE;
  }

  /* Build the CP9 HMM and associated data */
  /* IMPORTANT: do this before setting up CM for local mode
   * if we already have these, free them (wasteful but safe, 
   * and not a big deal b/c we 'should' only call ConfigCM() once
   * per CM.  
   */
  if(cm->cp9map     != NULL) FreeCP9Map(cm->cp9map);
  if(cm->cp9b       != NULL) FreeCP9Bands(cm->cp9b);
  if(cm->cp9        != NULL) FreeCPlan9(cm->cp9);
  if(cm->cp9_mx     != NULL) FreeCP9Matrix(cm->cp9_mx);
  if(cm->cp9_bmx    != NULL) FreeCP9Matrix(cm->cp9_bmx);

  if(!(build_cp9_hmm(cm, &(cm->cp9), &(cm->cp9map), FALSE, 0.0001, 0))) cm_Fail("Couldn't build a CP9 HMM from the CM\n");
  cm->cp9b = AllocCP9Bands(cm, cm->cp9);
  /* create the CP9 matrices, we init to 1 row, which is tiny so it's okay
   * that we have two of them, we only grow them as needed, cp9_bmx is 
   * only needed if we're doing Forward -> Backward -> Posteriors.
   */
  cm->cp9_mx  = CreateCP9Matrix(1, cm->cp9->M);
  cm->cp9_bmx = CreateCP9Matrix(1, cm->cp9->M);
  cm->flags |= CMH_CP9; /* raise the CP9 flag */
  
  /* Possibly configure the CM for local alignment. */

  if (cm->config_opts & CM_CONFIG_LOCAL)
    { 
      printf("About to call ConfigLocal, cm->pbegin %.5f cm->pend %.5f\n", cm->pbegin, cm->pend);
      ConfigLocal(cm, cm->pbegin, cm->pend);
      CMLogoddsify(cm);
    }
  /* Possibly configure the CP9 for local alignment
   * Note: CP9 local/glocal config does not necessarily match CM config 
   *       in fact cmsearch default is local CM, glocal CP9 */
  if((cm->config_opts & CM_CONFIG_HMMLOCAL) ||
     (cm->align_opts  & CM_ALIGN_SUB))
    {
      if(cm->align_opts & CM_ALIGN_SUB)
	{
	  /* To get spos and epos for the sub_cm, 
	   * we config the HMM to local mode with equiprobable start/end points.*/
	  swentry= ((cm->cp9->M)-1.)/cm->cp9->M; /* all start pts equiprobable, including 1 */
	  swexit = ((cm->cp9->M)-1.)/cm->cp9->M; /* all end   pts equiprobable, including M */
	}
      else
	{
	  swentry = cm->pbegin;
	  swexit  = cm->pbegin;
	  /*swentry= ((cm->cp9->M)-1.)/cm->cp9->M;*/ /* all start pts equiprobable, including 1 */
	  /*swexit = ((cm->cp9->M)-1.)/cm->cp9->M;*/ /* all end   pts equiprobable, including M */
	}
      CPlan9SWConfig(cm->cp9, swentry, swexit);
      CP9Logoddsify(cm->cp9);
    }
  if(cm->config_opts & CM_CONFIG_HMMEL)
    CPlan9ELConfig(cm);

  /*
    FILE *fp;
    fp = fopen("temphmm1" ,"w");
    debug_print_cp9_params(fp, cm->cp9, TRUE);
    fclose(fp);
    
    fp = fopen("tempcm1" ,"w");
    debug_print_cm_params(fp, cm);
    fclose(fp);
  */

  /* If nec, set up the query dependent bands, this has to be done after 
   * local is set up because we want to consider local begins, but NOT local
   * ends. This is inefficient, local ends are set up twice currently, once
   * before QDB calc, then turned off in BandCalculationEngine() then 
   * back on. This should be fixed. */
  if (do_calc_qdb)
    {
      if(cm->flags & CMH_QDB) cm_Fail("ERROR in ConfigCM() CM already has QDBs\n");
      ConfigQDB(cm);
    }
  else if(do_preset_qdb)
    {
      if(cm->flags & CMH_QDB) cm_Fail("ERROR in ConfigCM() CM already has QDBs\n");
      ESL_ALLOC(cm->dmin, sizeof(int) * cm->M);
      ESL_ALLOC(cm->dmax, sizeof(int) * cm->M);
      for(v = 0; v < cm->M; v++)
	{
	  cm->dmin[v] = preset_dmin[v];
	  cm->dmax[v] = preset_dmax[v];
	}
      /* Set W as dmax[0], we're wasting time otherwise, looking at
       * hits that are bigger than we're allowing with QDB. */
      cm->W = cm->dmax[0];
      cm->flags |= CMH_QDB; /* raise the QDB flag */
    }

  /*
  fp = fopen("temphmm2" ,"w");
  debug_print_cp9_params(fp, cm->cp9, TRUE);
  fclose(fp);

  fp = fopen("tempcm2" ,"w");
  debug_print_cm_params(fp, cm);
  fclose(fp);
  */

  /*cm_Fail("done.\n");*/
  /* We need to ensure that cm->el_selfsc * W >= IMPOSSIBLE
   * (cm->el_selfsc is the score for an EL self transition) This is
   * done because we potentially multiply cm->el_selfsc * W, and add
   * that to IMPOSSIBLE. 
   */
  if((cm->el_selfsc * cm->W) < IMPOSSIBLE)
    { 
      cm->el_selfsc = (IMPOSSIBLE / (cm->W+1));
      cm->iel_selfsc = -INFTY;
    }
  /* Potentially, overwrite transitions with non-probabilistic 
   * RSEARCH transitions, we do this after setting up QDBs and
   * the CP9 HMM, so they'll correspond to the probabilistic 
   * transitions that existed prior to overwriting with RSEARCH
   * transitions. Transitions scores are overwritten in CMLogoddsify() 
   */
  CMLogoddsify(cm);
  if(cm->config_opts & CM_CONFIG_ZEROINSERTS)
    CMHackInsertScores(cm);	    /* insert emissions are all equiprobable,
				     * makes all CP9 (if non-null) inserts equiprobable */
  /*debug_print_cm_params(stdout, cm);
    debug_print_cp9_params(stdout, cm->cp9, TRUE);*/
  return eslOK;

 ERROR:
  cm_Fail("Memory allocation error.");
  return status; /* NOTREACHED */
}

/*
 * Function: ConfigCMEnforce
 * Date:     EPN, Wed Feb 14 12:57:21 2007
 * Purpose:  Configure a CM for enforcing a subsequence for search or 
 *           alignment. 
 * 
 * Args:     CM           - the covariance model
 */
void
ConfigCMEnforce(CM_t *cm)
{
  int do_build_cp9  = FALSE;
  int enf_start_pos;            /* consensus left position node enf_start emits to   */
  int enf_end_pos;              /* consensus left position node enf_end   emits to   */
  int enf_end;                  /* last node we're enforcing                         */
  CMEmitMap_t *emap;            /* consensus emit map for the CM, used iff enforcing */
  float nonenf_sc;              /* score of cm->enfseq we're about to enforce before *
				 * we reparameterize the CM                          */
  float enf_sc;                 /* score of cm->enfseq subseq after CM is            *
				 * reparameterized to enforce it                     */

  /* Contract checks */
  if(!(cm->config_opts & CM_CONFIG_ENFORCE))
    cm_Fail("ERROR in ConfigCMEnforce() trying to enforce a subsequence but CM_CONFIG_ENFORCE flag is down.");
  if(cm->flags & CM_ENFORCED)
    cm_Fail("ERROR in ConfigCMEnforce() trying to enforce a subsequence but CM_IS_ENFORCED flag is up.");
  /* Can't enforce in RSEARCH mode yet */  
  if(cm->flags & CM_IS_RSEARCH)
    cm_Fail("ERROR in ConfigCMEnforce() trying to enforce a subsequence in RSEARCH mode, not yet implemented.");
  /* Can't enforce in sub mode */  
  if(cm->align_opts & CM_ALIGN_SUB)
    cm_Fail("ERROR in ConfigCMEnforce() can't enforce a subsequence in sub alignment mode.");

  /* First, get the score of the enforced subseq for the non-enforced model */
  nonenf_sc = EnforceScore(cm);

  /* IMPORTANT: if CM has local begins, make it global, we'll relocalize 
   * it later based on cm->config_opts, cm->search_opts, and/or cm->align_opts,
   * we need to do this so we can build a CP9 (which can't be done with local CMs yet)*/
  if(cm->flags & CMH_LOCAL_BEGIN)
    ConfigGlobal(cm);

  /* Enforce the sequence */
  EnforceSubsequence(cm);

  /* if we have a CP9, free it, and build a new one, (this one will automatically
   * have the subseq enforced b/c it's built from the reparam'ized CM) */
  if(cm->flags & CMH_CP9)
    {
      FreeCPlan9(cm->cp9);
      cm->flags &= ~CMH_CP9; /* drop the CP9 flag */
      do_build_cp9 = TRUE;
    }
  else if (cm->config_opts & CM_CONFIG_ENFORCEHMM)
    {
      /* we didn't have a CP9 before, but we need one now */
      do_build_cp9 = TRUE;
    }
  if(do_build_cp9)
    {
      if(!(build_cp9_hmm(cm, &(cm->cp9), &(cm->cp9map), 
			TRUE, /* b/c we're enforcing, check CP9 mirrors CM */
			0.0001, 0)))
	cm_Fail("Couldn't build a CP9 HMM from the CM\n");
      cm->flags |= CMH_CP9; /* raise the CP9 flag */
    }

  /* Configure the CM for local alignment . */
  if (cm->config_opts & CM_CONFIG_LOCAL)
    { 
      ConfigLocalEnforce(cm, cm->pbegin, cm->pend); /* even in local we require each parse 
						     * go through the enforced subseq */
      CMLogoddsify(cm);
      if(cm->config_opts & CM_CONFIG_ZEROINSERTS)
	CMHackInsertScores(cm);	    /* insert emissions are all equiprobable,
				     * makes all CP9 (if non-null) inserts equiprobable */
    }
  /* Possibly configure the CP9 for local alignment
   * Note: CP9 local/glocal config does not necessarily match CM config 
   *       in fact cmsearch default is local CM, glocal CP9 */
  if((cm->flags & CMH_CP9) && (cm->config_opts & CM_CONFIG_HMMLOCAL))
    {
      /* Set up the CP9 locality to enforce a subseq */
      emap = CreateEmitMap(cm); 
      enf_end = cm->enf_start + strlen(cm->enf_seq) - 1;
      enf_start_pos = emap->lpos[cm->enf_start];
      enf_end_pos   = emap->lpos[enf_end];
      FreeEmitMap(emap);
      CPlan9SWConfigEnforce(cm->cp9, cm->pbegin, cm->pbegin, enf_start_pos, enf_end_pos);
      CP9Logoddsify(cm->cp9);
    }
     
  if(cm->config_opts & CM_CONFIG_ZEROINSERTS)
    CMHackInsertScores(cm);	    /* insert emissions are all equiprobable,
				     * makes all CP9 (if non-null) inserts equiprobable*/

  if(cm->config_opts & CM_CONFIG_ENFORCEHMM)
    {
      if(!(cm->flags & CMH_CP9))
	cm_Fail("ERROR trying to configure the HMM for naive enforcement, but the cm's CMH_CP9 flag is down.\n");
      /* We make the HMM ignorant of any sequence conservation besides
       * the enforced subseq. This way ALL subseqs with the enforced
       * subseq will be recognized as high scoring by the HMM and 
       * be passed to the CM (if filtering (which is default in this mode)).
       * To achieve this, make all emissions (match and insert) score 0,
       * except for the few match emissions that model the enforced subseq */
      emap = CreateEmitMap(cm); 
      enf_end = cm->enf_start + strlen(cm->enf_seq) - 1;
      enf_start_pos = emap->lpos[cm->enf_start];
      enf_end_pos   = emap->lpos[enf_end];
      FreeEmitMap(emap);
      CP9HackInsertScores(cm->cp9);
      CP9EnforceHackMatchScores(cm->cp9, enf_start_pos, enf_end_pos);
    }	

  /* Determine the score of the enforced subseq for the enforced model */
  enf_sc = EnforceScore(cm);
  
  cm->enf_scdiff = enf_sc - nonenf_sc;
  cm->flags |= CM_ENFORCED; /* raise the enforced flag */

  if(cm->flags & CMH_SCANMATRIX)
    cm->flags &= ~CMH_SCANMATRIX; /* enforcement invalidates ScanMatrix */

  return; 
}

/*
 * Function: ConfigLocal
 * Purpose:  Configure a CM for local alignment by spreading 
 *           p_internal_start local entry probability evenly
 *           across all internal nodes, and by spreading
 *           p_internal_exit local exit probability evenly
 *           across all internal nodes.
 * 
 * Args:     cm               - the covariance model
 *           p_internal_start - prob mass to spread for local begins
 *           p_internal_exit  - prob mass to spread for local ends
 */        

void
ConfigLocal(CM_t *cm, float p_internal_start, float p_internal_exit)
{
  int status;
  int v;			/* counter over states */
  int nd;			/* counter over nodes */
  int nstarts;			/* number of possible internal starts */
  int had_scanmatrix;           /* true if CM had a scan matrix when function was entered */

  /* contract check */
  if(cm->flags & CMH_LOCAL_BEGIN)
    cm_Fail("ERROR in ConfigLocal(), CMH_LOCAL_BEGIN flag already up.\n");
  if(cm->flags & CMH_LOCAL_END)
    cm_Fail("ERROR in ConfigLocal(), CMH_LOCAL_END flag already up.\n");

  if(cm->flags & CMH_SCANMATRIX) had_scanmatrix = TRUE;
  else had_scanmatrix = FALSE;

  /*****************************************************************
   * Internal entry.
   *****************************************************************/
  /* Count "internal" nodes: MATP, MATL, MATR, and BIF nodes.
   * Ignore all start nodes, and also node 1 (which is always the
   * "first" node and gets an entry prob of 1-p_internal_start).
   */
  nstarts = 0;
  for (nd = 2; nd < cm->nodes; nd++) {
    if (cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd ||
    	cm->ndtype[nd] == MATR_nd || cm->ndtype[nd] == BIF_nd) 
      nstarts++;
  }

  /* Zero everything.
   */
  for (v = 0; v < cm->M; v++)  cm->begin[v] = 0.;

  /* Erase the previous transition p's from node 0. The only
   * way out of node 0 is going to be local begin transitions
   * from the root v=0 directly to MATP_MP, MATR_MR, MATL_ML,
   * and BIF_B states.
   *
   * EPN, Wed Feb 14 13:56:02 2007: First we want to save
   * the transition probs we're about to zero, so we can
   * globalize this model later if we have to.
   * cm->root_trans is NULL prior to this, so 
   * if we call ConfigGlobal() w/o executing this code,
   * we'll die (with an EASEL contract exception).
   */
  if(cm->root_trans == NULL) /* otherwise they've already been set */
    {
      ESL_ALLOC(cm->root_trans, sizeof(float) * cm->cnum[0]);
      for (v = 0; v < cm->cnum[0]; v++)
	cm->root_trans[v] = cm->t[0][v];
    }
  for (v = 0; v < cm->cnum[0]; v++) cm->t[0][v] = 0.;

  /* Node 1 gets prob 1-p_internal_start.
   */
  cm->begin[cm->nodemap[1]] = 1.-p_internal_start;

  /* Remaining nodes share p_internal_start.
   */
  for (nd = 2; nd < cm->nodes; nd++) {
    if (cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd ||
    	cm->ndtype[nd] == MATR_nd || cm->ndtype[nd] == BIF_nd)  
      cm->begin[cm->nodemap[nd]] = p_internal_start/(float)nstarts;
  }
  cm->flags |= CMH_LOCAL_BEGIN;
  
  /*****************************************************************
   * Internal exit.
   *****************************************************************/
  ConfigLocalEnds(cm, p_internal_exit);

  /* new local probs invalidate log odds scores and QDBs */
  cm->flags &= ~CMH_BITS;
  /* recalc QDBs if they exist */
  if(cm->flags & CMH_QDB) {
    free(cm->dmin);
    free(cm->dmax);
    cm->dmin = NULL;
    cm->dmax = NULL;
    cm->flags &= ~CMH_QDB;
    ConfigQDB(cm);
  }      
  /* ConfigQDB should rebuild scan matrix, if it existed */
  if((! (cm->flags & CMH_SCANMATRIX)) && had_scanmatrix)
     cm_Fail("ConfigLocal(), CM had a scan matrix, but ConfigQDB didn't rebuild it.");
     
  CMLogoddsify(cm);
  if(cm->config_opts & CM_CONFIG_ZEROINSERTS)
    CMHackInsertScores(cm);	    /* insert emissions are all equiprobable,
				     * makes all CP9 (if non-null) inserts equiprobable */
  return;

 ERROR:
  cm_Fail("Memory allocation error.");
}

/*
 * Function: ConfigGlobal
 * Purpose:  Configure a CM in local alignment mode to global
 *           alignment mode.
 *
 * Args:     CM               - the covariance model
 */        

void
ConfigGlobal(CM_t *cm)
{
  int v;			/* counter over states */

  /*printf("in configGlobal\n");*/
  /* Contract check: local begins MUST be active, if not then cm->root_trans (the 
   * transition probs from state 0 before local configuration) will be NULL, 
   * so we can't copy them back into cm->t[0], which is a problem. This is fragile. */
  if(!(cm->flags & CMH_LOCAL_BEGIN))
    cm_Fail("ERROR in ConfigGlobal() trying to globally configure a CM that has no local begins.");
  if(!(cm->flags & CMH_LOCAL_END))
    cm_Fail("ERROR in ConfigGlobal() trying to globally configure a CM that has no local ends.");
  if(cm->root_trans == NULL)
    cm_Fail("ERROR in ConfigGlobal() cm->root_trans NULL. CM must have been configured with local begins before we can configure it back to global");
  
  /*****************************************************************
   * Make local begins impossible
   *****************************************************************/
  for (v = 0; v < cm->M; v++)  cm->begin[v] = 0.;
  /* Now reset transitions out of ROOT_S to their initial state, 
   * ConfigLocal() zeroes these guys, but they've been saved in 
   * the CM data structure in (C
   * in the CM data structure. */
  for (v = 0; v < cm->cnum[0]; v++)  cm->t[0][v] = cm->root_trans[v];
  /*printf("in ConfigGlobal, printing transitions from the root.\n");
    for (v = 0; v < cm->cnum[0]; v++)  printf("cm->t[0][v:%d]: %f\n", v, cm->t[0][v]);*/

  cm->flags &= ~CMH_LOCAL_BEGIN; /* drop the local begin flag */
  
  /*****************************************************************
   * Make local ends impossible
   *****************************************************************/
  ConfigNoLocalEnds(cm);

  /* new probs invalidate log odds scores and QDB */
  cm->flags &= ~CMH_BITS;
  /* Recalc QDBs if they exist */
  if(cm->flags & CMH_QDB) {
    free(cm->dmin);
    free(cm->dmax);
    cm->dmin = NULL;
    cm->dmax = NULL;
    cm->flags &= ~CMH_QDB;
    ConfigQDB(cm);
  }      
  /* free and rebuild scan matrix to correspond to new QDBs, if it exists */
  if(cm->flags & CMH_SCANMATRIX) {
    int do_float = cm->smx->flags & cmSMX_HAS_FLOAT;
    int do_int   = cm->smx->flags & cmSMX_HAS_INT;
    cm_FreeScanMatrixForCM(cm);
    cm_CreateScanMatrixForCM(cm, do_float, do_int);
  }

  CMLogoddsify(cm);
  if(cm->config_opts & CM_CONFIG_ZEROINSERTS)
    CMHackInsertScores(cm);	    /* insert emissions are all equiprobable,
				     * makes all CP9 (if non-null) inserts equiprobable */
  return;
}

/**************************************************************************
 * EPN 10.02.06
 * Function: ConfigNoLocalEnds()
 *
 * Purpose:  Set the probability of local ends to 0.0 for all states.
 *           This function was introduced for use in BandCalculationEngine()
 *           because allowing local ends when calculating bands dramatically
 *           widens all bands and decreases search acceleration. So this
 *           is the ad-hoc fix.                    
 * 
 * Args:    
 * CM_t *cm               - the CM
 * Returns: (void) 
 */
void
ConfigNoLocalEnds(CM_t *cm)
{
  int v;			/* counter over states */
  int nd;                       /* counter over nodes  */

  /* Contract check */
  if(!(cm->flags & CMH_LOCAL_END))
    cm_Fail("ERROR in ConfigNoLocalEnds() CMH_LOCAL_END flag already down.\n");

  for (v = 0; v < cm->M; v++) cm->end[v] = 0.;
  /* Now, renormalize transitions */
  for (nd = 1; nd < cm->nodes; nd++) {
    if ((cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd ||
	 cm->ndtype[nd] == MATR_nd || cm->ndtype[nd] == BEGL_nd ||
	 cm->ndtype[nd] == BEGR_nd) && 
	cm->ndtype[nd+1] != END_nd)
      {
	v = cm->nodemap[nd];
	esl_vec_FNorm(cm->t[v], cm->cnum[v]);
      }
  }
  /* Disable the local end probs in the CP9 */
  if((cm->flags |= CMH_CP9) && (cm->cp9->flags |= CPLAN9_EL))
    CPlan9NoEL(cm);

  cm->flags &= ~CMH_LOCAL_END; /* turn off local ends flag */
  /* new probs invalidate log odds scores */
  cm->flags &= ~CMH_BITS;
  /* local end changes don't invalidate QDBs, which means
   * they don't affect the ScanMatrix either. 
   */

  return;
}

/*
 * Function: ConfigLocalEnds()
 * Date:     EPN 10.02.06
 * Purpose:  Given a probability of local ends, spread the probability of
 *           local ends evenly across all states from which local ends are
 *           permitted (see code).
 */

void
ConfigLocalEnds(CM_t *cm, float p_internal_exit)
{
  int v;			/* counter over states */
  int nd;			/* counter over nodes */
  int nexits;			/* number of possible internal ends */
  float denom;

  /* Contract check */
  if(cm->flags & CMH_LOCAL_END)
    cm_Fail("ERROR in ConfigLocalEnds() CMH_LOCAL_END flag already up.\n");

  /* Count internal nodes MATP, MATL, MATR, BEGL, BEGR that aren't
   * adjacent to END nodes.
   */
  nexits = 0;
  for (nd = 1; nd < cm->nodes; nd++) {
    if ((cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd ||
	 cm->ndtype[nd] == MATR_nd || cm->ndtype[nd] == BEGL_nd ||
	 cm->ndtype[nd] == BEGR_nd) && 
	cm->ndtype[nd+1] != END_nd)
      nexits++;
  } 
  /* Spread the exit probability across internal nodes.
   * Currently does not compensate for the decreasing probability
   * of reaching a node, the way HMMER does: therefore the probability
   * of exiting at later nodes is actually lower than the probability 
   * of exiting at earlier nodes. This should be a small effect.
   */
  for (v = 0; v < cm->M; v++) cm->end[v] = 0.;
  for (nd = 1; nd < cm->nodes; nd++) {
    if ((cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd ||
	 cm->ndtype[nd] == MATR_nd || cm->ndtype[nd] == BEGL_nd ||
	 cm->ndtype[nd] == BEGR_nd) && 
	cm->ndtype[nd+1] != END_nd)
      {
	v = cm->nodemap[nd];
	cm->end[v] = p_internal_exit / (float) nexits;
				/* renormalize the main model transition distribution */
	denom = esl_vec_FSum(cm->t[v], cm->cnum[v]);
	denom += cm->end[v];
	esl_vec_FScale(cm->t[v], cm->cnum[v], 1./denom);
	/* cm->t[v] vector will purposefully no longer sum to 1., 
	 * if we were to append cm->end[v] as a new number in the vector, it would sum to 1. 
	 */
      }
  }

  cm->flags |= CMH_LOCAL_END;

  /* new probs invalidate log odds scores */
  cm->flags &= ~CMH_BITS;
  /* local end changes don't invalidate QDBs, which means
   * they don't affect the ScanMatrix either. 
   */

  return;
}

/*
 * Function: ConfigLocal_DisallowELEmissions()
 * Purpose:  Silence the EL state.
 */
void
ConfigLocal_DisallowELEmissions(CM_t *cm)
{
  /* Set the EL self transition score to as close to IMPOSSIBLE 
   * as we can while still guaranteeing we won't get underflow errors.
   * we need cm->el_selfsc * W >= IMPOSSIBLE 
   * because we will potentially multiply cm->el_selfsc * W, and add that to 
   * 2 * IMPOSSIBLE, and IMPOSSIBLE must be > -FLT_MAX/3 so we can add it together 3 
   * times (see structs.h). 
   */
  cm_Fail("ConfigLocal_DisallowELEmissions is deprecated.");
  cm->el_selfsc = (IMPOSSIBLE / (cm->W+1));
  cm->iel_selfsc = -INFTY; 
  cm->iel_selfsc = -100 * INTSCALE; 
  return;
}

/* EPN, Thu Jan  4 10:10:07 2007
 * 
 * Function: ConfigLocalEnforce
 * 
 * Purpose:  Given a CM with valid cm->enf_start and cm->enf_seq variables,
 *           modify local entries and exits so that the nodes starting
 *           at cm->enf_start and going to cm->enf_start + strlen(cm->enf_seq)
 *           must be entered, i.e. disallow any local pass that omits them.
 * 
 * Args:     CM           - the covariance model
 *           p_internal_start - total prob of a local begin to spread 
 *           p_internal_exit  - total prob of a local end to spread
 */
void
ConfigLocalEnforce(CM_t *cm, float p_internal_start, float p_internal_exit)
{
  int v;			/* counter over states */
  int nd;			/* counter over nodes */
  int nstarts;			/* number of possible internal starts */
  int enf_start_pos;            /* consensus left position node enf_start emits to */
  int enf_end_pos;              /* consensus left position node enf_end   emits to */
  int nexits;			/* number of possible internal ends */
  float denom;
  CMEmitMap_t *emap;            /* consensus emit map for the CM */
  int enf_end;

  /* Contract checks */
  if(cm->enf_seq == NULL || cm->enf_start == 0)
    cm_Fail("ERROR, in ConfigLocalEnforce, but no subseq to enforce.\n");
  if(cm->flags & CMH_LOCAL_BEGIN)
    cm_Fail("ERROR in ConfigLocalEnforce() CMH_LOCAL_BEGIN flag already up.\n");
  if(cm->flags & CMH_LOCAL_END)
    cm_Fail("ERROR in ConfigLocalEnforce() CMH_LOCAL_END flag already up.\n");

  enf_end = cm->enf_start + strlen(cm->enf_seq) - 1;
  /* We want every parse to go through the MATL stretch from enf_start
   * to enf_end. To enforce this we disallow local begin and ends that
   * would allow parses to miss these nodes. */
  for(nd = cm->enf_start; nd <= enf_end; nd++)
    {
      if(cm->ndtype[nd] != MATL_nd)
	cm_Fail("ERROR, trying to enforce a non-MATL stretch (node: %d not MATL).\n", nd);
    }
  emap = CreateEmitMap(cm); /* diff from ConfigLocalEnds() */
  enf_start_pos = emap->lpos[cm->enf_start];
  enf_end_pos   = emap->lpos[enf_end];

  /* The following code is copied from ConfigLocal() and ConfigLocalEnds()
   * with modification to disallow local begins before enf_start and 
   * disallow exits from between closest_start and enf_end. This 
   * implementation sets local entry to the first node as 1-p_internal_start,
   * and end from final node as 1-p_internal_exit. */

  /*****************************************************************
   * Internal entry.
   *****************************************************************/
  /* Count "internal" nodes: MATP, MATL, MATR, and BIF nodes.
   * Ignore all start nodes, and also node 1 (which is always the
   * "first" node and gets an entry prob of 1-p_internal_start).
   */
  nstarts = 0;
  for (nd = 2; nd < cm->nodes; nd++) {
    if (cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd ||
    	cm->ndtype[nd] == MATR_nd || cm->ndtype[nd] == BIF_nd) 
      if(emap->lpos[nd] <= enf_start_pos &&
	 emap->rpos[nd] >= enf_end_pos) /* diff from ConfigLocalEnds() */
	nstarts++;
  }

  /* Zero everything.
   */
  for (v = 0; v < cm->M; v++)  cm->begin[v] = 0.;

  /* Erase the previous transition p's from node 0. The only
   * way out of node 0 is going to be local begin transitions
   * from the root v=0 directly to MATP_MP, MATR_MR, MATL_ML,
   * and BIF_B states.
   */
  for (v = 0; v < cm->cnum[0]; v++)  cm->t[0][v] = 0.;

  /* Node 1 gets prob 1-p_internal_start.
   */
  cm->begin[cm->nodemap[1]] = 1.-p_internal_start;

  /* Remaining nodes share p_internal_start.
   */
  for (nd = 2; nd < cm->nodes; nd++) 
    {
      if (cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd ||
	  cm->ndtype[nd] == MATR_nd || cm->ndtype[nd] == BIF_nd)  
	{
	  if(emap->lpos[nd] <= enf_start_pos &&
	     emap->rpos[nd] >= enf_end_pos) /* diff from ConfigLocalEnds() */
	    {
	      /*printf("enabling local begin into nd: %d lpos: %d rpos: %d s: %d e: %d\n", nd, emap->lpos[nd], emap->rpos[nd], enf_start_pos, enf_end_pos);*/
	      cm->begin[cm->nodemap[nd]] = p_internal_start/(float)nstarts;
	    }
	  else
	    ;/*printf("NOT enabling local begin into nd: %d lpos: %d rpos: %d s: %d e: %d\n", nd, emap->lpos[nd], emap->rpos[nd], enf_start_pos, enf_end_pos);*/
	}
    }
  cm->flags |= CMH_LOCAL_BEGIN;
  
  /*****************************************************************
   * Internal exit.
   *****************************************************************/
  /* Count internal nodes MATP, MATL, MATR, BEGL, BEGR that aren't
   * adjacent to END nodes.
   */
  nexits = 0;
  for (nd = 1; nd < cm->nodes; nd++) {
    if ((cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd ||
	 cm->ndtype[nd] == MATR_nd || cm->ndtype[nd] == BEGL_nd ||
	 cm->ndtype[nd] == BEGR_nd) && 
	cm->ndtype[nd+1] != END_nd)
      if(emap->lpos[nd] >= enf_end_pos || 
	 emap->rpos[nd] <  enf_start_pos) /* diff from ConfigLocalEnds() */
	nexits++;
  } 
  /* Spread the exit probability across internal nodes.
   * Currently does not compensate for the decreasing probability
   * of reaching a node, the way HMMER does: therefore the probability
   * of exiting at later nodes is actually lower than the probability 
   * of exiting at earlier nodes. This should be a small effect.
   */
  for (v = 0; v < cm->M; v++) cm->end[v] = 0.;
  for (nd = 1; nd < cm->nodes; nd++) {
    if ((cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd ||
	 cm->ndtype[nd] == MATR_nd || cm->ndtype[nd] == BEGL_nd ||
	 cm->ndtype[nd] == BEGR_nd) && 
	cm->ndtype[nd+1] != END_nd)
      {
	v = cm->nodemap[nd];
	if(emap->lpos[nd] >= enf_end_pos || 
	   emap->rpos[nd] <  enf_start_pos) /* diff from ConfigLocalEnds() */
	  {
	    /*printf("enabling local end from nd: %d lpos: %d rpos: %d s: %d e: %d\n", nd, emap->lpos[nd], emap->rpos[nd], enf_start_pos, enf_end_pos);*/
	    cm->end[v] = p_internal_exit / (float) nexits;
	  }
	else
	  {
	    ;/*printf("NOT enabling local end from nd: %d lpos: %d rpos: %d s: %d e: %d\n", nd, emap->lpos[nd], emap->rpos[nd], enf_start_pos, enf_end_pos);*/
	  }
	/* renormalize the main model transition distribution,
	 * it's important to do this for all states that
	 * may have had a local end possible prior to this function call*/
	denom = esl_vec_FSum(cm->t[v], cm->cnum[v]);
	denom += cm->end[v];
	esl_vec_FScale(cm->t[v], cm->cnum[v], 1./denom);
      }
  }
  cm->flags |= CMH_LOCAL_END;
  FreeEmitMap(emap);

  return;
}

/*******************************************************************************
 * Function: EnforceSubsequence()
 * Date:     EPN, Thu Jan  4 10:13:08 2007
 * Purpose:  Modify CM probabilities so that if a particular subsequence (cm->enf_subseq)
 *           is not emitted, a big bit score penalty is incurred. Specifically designed
 *           for enforcing the telomerase RNA template sequence.
 */
int  
EnforceSubsequence(CM_t *cm)
{
  int status;
  int nd;
  float small_chance = 1e-15; /* any parse not including the enforced path includes
			       * an emission or transition with a -45 bit score */
  int   enf_end;
  int v;
  int a;
  float *nt;
  ESL_SQ *enf_sq = NULL;     /* We'll fill this with enf_seq and digitize it */

  ESL_ALLOC(nt, sizeof(float) * cm->abc->K);

  enf_end = cm->enf_start + strlen(cm->enf_seq) - 1;
  /*printf("in EnforceSubsequence, start posn: %d cm->enf_seq: %s\n", cm->enf_start, cm->enf_seq);*/
  for(nd = (cm->enf_start-1); nd <= enf_end; nd++)
    {
      if(cm->ndtype[nd] != MATL_nd)
	cm_Fail("ERROR, trying to enforce a non-MATL stretch (node: %d not MATL).\n", nd);
    }

  /* Go through each node and enforce the template by changing the
   * emission and transition probabilities as appropriate. */

  /* First deal with node before cm->enf_start, we want to ensure that cm->enf_start is
   * entered. We know cm->enf_start - 1 and cm->enf_start are both MATL nodes */
  nd = cm->enf_start - 1;
  v  = cm->nodemap[nd];       /* MATL_ML*/
  cm->t[v][2] = small_chance; /* ML->D  */
  v++;                        /* MATL_D */
  cm->t[v][2] = small_chance; /*  D->D  */
  v++;                        /* MATL_IL*/
  cm->t[v][2] = small_chance; /*  IL->D */

  /* Now move on to the MATL nodes we're enforcing emits the cm->enf_seq */
  enf_sq  = esl_sq_CreateFrom("enforced", cm->enf_seq, NULL, NULL, NULL);
  esl_sq_Digitize(cm->abc, enf_sq);

  for(nd = cm->enf_start; nd <= enf_end; nd++) 
    {
      /*printf("enforcing subseq for node: %d\n", nd);*/
      /* Enforce the transitions, unless we're the last node of the stretch */
      v  = cm->nodemap[nd];       /* MATL_ML*/
      if(nd < enf_end)
	{
	  cm->t[v][0] = small_chance; /* ML->IL */
	  cm->t[v][2] = small_chance; /* ML->D  */
	}
      /* Enforce the emission. Taking into account ambiguities. */
      esl_vec_FSet(nt, cm->abc->K, 0.);
      /*printf("enf_dsq[%d]: %d\n", (nd-cm->enf_start+1), (int) (enf_dsq[(nd-cm->enf_start+1)]));*/
      esl_abc_FCount(cm->abc, nt, enf_sq->dsq[(nd - cm->enf_start + 1)], 1.);
      /* nt is now a count vector norm'ed to 1.0 with relative contributions 
       * of each (A,C,G,U) nucleotides towards the (potentially ambiguous)
       * residue in enf_dsq[(nd-cm->enf_start+1)]) 
       */

      for(a = 0; a < cm->abc->K; a++)
	{
	  /* start out by setting each residue to 'small_chance' */
	  cm->e[v][a] =  small_chance;
	  cm->e[v][a] += nt[a];
	}
    }
  free(nt);
  esl_sq_Destroy(enf_sq);

  CMRenormalize(cm);
  /* new probs invalidate log odds scores */
  cm->flags &= ~CMH_BITS;
  /* Recalc QDBs if they exist */
  if(cm->flags & CMH_QDB) {
    free(cm->dmin);
    free(cm->dmax);
    cm->dmin = NULL;
    cm->dmax = NULL;
    cm->flags &= ~CMH_QDB;
    ConfigQDB(cm);
  }      
  /* free and rebuild scan matrix to correspond to new QDBs, if it exists */
  if(cm->flags & CMH_SCANMATRIX) {
    int do_float = cm->smx->flags & cmSMX_HAS_FLOAT;
    int do_int   = cm->smx->flags & cmSMX_HAS_INT;
    cm_FreeScanMatrixForCM(cm);
    cm_CreateScanMatrixForCM(cm, do_float, do_int);
  }

  CMLogoddsify(cm); /* QDB calculation invalidates log odds scores */
  if(cm->config_opts & CM_CONFIG_ZEROINSERTS)
    CMHackInsertScores(cm);	    /* insert emissions are all equiprobable,
				     * makes all CP9 (if non-null) inserts equiprobable */

  /*for(nd = cm->enf_start; nd <= enf_end; nd++) 
    {
      v  = cm->nodemap[nd];      
      for(a = 0; a < cm->abc->K; a++)
	printf("cm->e[v:%d][a:%d]: %f sc: %f\n", v, a, cm->e[v][a], cm->esc[v][a]);
    }
  printf("\n");*/

  return eslOK;

 ERROR: 
  cm_Fail("Memory allocation error.\n");
  return status; /* never reached */
}

/*******************************************************************************
 * Function: EnforceScore()
 * Date:     EPN, Wed Feb 14 16:19:22 2007
 * Purpose:  Determine the subparse score of aligning cm->enfseq to the MATL_ML 
 *           states of consecutive MATL nodes starting at cm->enfstart. This
 *           function can be called before and after enforcing the subseq 
 *           via reparameterization of the relevant nodes, to determine the
 *           score difference of cm->enfseq b/t the non-enforced and enforced
 *           CMs.
 */
float
EnforceScore(CM_t *cm)
{
  ESL_SQ *enf_sq;/* a digitized version of cm->enf_seq */
  int   enf_end; /* last node to be enforced */
  int   nd;      /* node index  */
  int   v;       /* state index */
  int   i;       /* sequence position index */
  float score;   /* score of subparse that starts in first MATL_ML of enforced stretch,
		  * goes through each MATL_ML and emits the enforced residues (which
		  * can be ambiguous). */

  /* Contract check. */
  if(!(cm->config_opts & CM_CONFIG_ENFORCE))
    cm_Fail("ERROR in EnforceScore(), cm->config_opt CM_CONFIG_ENFORCE not raised.\n");

  enf_end = cm->enf_start + strlen(cm->enf_seq) - 1;
  /*printf("in EnforceScore(), start posn: %d cm->enf_seq: %s\n", cm->enf_start, cm->enf_seq);*/
  for(nd = (cm->enf_start-1); nd <= enf_end; nd++)
    {
      if(cm->ndtype[nd] != MATL_nd)
	cm_Fail("ERROR, trying to enforce a non-MATL stretch (node: %d not MATL).\n", nd);
    }

  /* Go through each node and determine the score of the subparse that
   * goes through the nodes that are/will be enforced.  To start, we
   * have to transit to MATL_ML of cm->enf_start from either MATL_ML
   * of MATL_ML or MATL_IL of nd=cm->enf_start-1, but we don't know
   * which.  Can't think of robust way of handling this, current
   * strategy is to take the average of the two transition scores.
   * (this is hacky, but should have small effect on cm->enf_scdiff).
   */
  nd = cm->enf_start - 1;
  v  = cm->nodemap[nd];       /* MATL_ML*/
  score =  (cm->tsc[v][1] + cm->tsc[v+2][1]) / 2;
  /*printf("init v: %d ML->ML: %f IL->ML: %f avg: %f\n", v, cm->tsc[v][1], cm->tsc[(v+2)][1], score);*/

  /* Now move on to the MATL nodes we're enforcing emits the cm->enf_seq */
  enf_sq  = esl_sq_CreateFrom("enforced", cm->enf_seq, NULL, NULL, NULL);
  esl_sq_Digitize(cm->abc, enf_sq);

  for(nd = cm->enf_start; nd <= enf_end; nd++) 
    {
      i = nd - cm->enf_start+1; /* enf_dsq goes 1..(strlen(cm->enf_seq)) 
				 * bordered by sentinels */
      /* Add score for the MATL_ML->MATL_ML transition, 
       * unless we're the last node of the stretch */
      v  = cm->nodemap[nd];       /* MATL_ML*/
      if(nd < enf_end)
	score += cm->tsc[v][1]; /* ML->ML */

      /* Add score for the emission. Taking into account ambiguities. */
      if (enf_sq->dsq[i] < cm->abc->K)
	score += cm->esc[v][enf_sq->dsq[i]];
      else
	score += esl_abc_FAvgScore(cm->abc, enf_sq->dsq[i], cm->esc[v]);

    }
  /*printf("in EnforceScore() returning sc: %f\n", score);*/
  esl_sq_Destroy(enf_sq);

  return score;
}

/*******************************************************************************
 * Function: EnforceFindEnfStart()
 * Date:     EPN, Fri Feb  9 10:32:44 2007
 * Purpose:  Determine the node cm->enf_start given the consensus column it 
 *           models, and check that it's a MATL node (this requirement could 
 *           be relaxed in the future).
 * Returns:  (int) the CM MATL node index that emits to consensus column 
 *           enf_cc_start. Dies if there's no such node.
 */
int  
EnforceFindEnfStart(CM_t *cm, int enf_cc_start)
{
  CMEmitMap_t *emap;            /* consensus emit map for the CM */
  int enf_start;                /* CM MATL node that emits to enf_cc_start */
  int nd;                       /* counter over nodes */
  
  emap      = CreateEmitMap(cm); 
  enf_start = -1;
  if(enf_cc_start > emap->clen)
    cm_Fail("ERROR --enfstart <n>, there's only %d columns, you chose column %d\n", 
	enf_cc_start, emap->clen);
  for(nd = 0; nd < cm->nodes; nd++)
    {
      if(emap->lpos[nd] == enf_cc_start) 
	{
	  if(cm->ndtype[nd] == MATL_nd)	      
	    {
	      enf_start = nd;
	      break;
	    }
	  else if(cm->ndtype[nd] == MATP_nd)	      
	    cm_Fail("ERROR --enfstart <n>, <n> must correspond to MATL modelled column\nbut %d is modelled by a MATP node.\n", enf_cc_start);
	}
      else if(emap->rpos[nd] == enf_cc_start)
	{
	  if(cm->ndtype[nd] == MATR_nd)	      
	    cm_Fail("ERROR --enfstart <n>, <n> must correspond to MATL modelled column\nbut %d is modelled by a MATR node.\n", enf_cc_start);
	  if(cm->ndtype[nd] == MATP_nd)	      
	    cm_Fail("ERROR --enfstart <n>, <n> must correspond to MATL modelled column\nbut %d is modelled by the right half of a MATP node.\n", enf_cc_start);
	}	      
    }
  if(enf_start == -1)
    cm_Fail("ERROR trying to determine the start node for the enforced subsequence.\n");
  FreeEmitMap(emap);
  return(enf_start);
}


/*
 * Function: ConfigQDB
 * Date:     EPN, Thu May  3 14:37:09 2007
 * Purpose:  Configure a CM's query dependent bands (QDBs).
 * Args:
 *           CM           - the covariance model
 */
int
ConfigQDB(CM_t *cm)
{
  int safe_windowlen;

  /* Contract check */
  if(cm->flags & CMH_QDB)
    cm_Fail("ERROR in ConfigQDB() CMH_QDB flag already up.\n");

  safe_windowlen = cm->W * 2;
  if(cm->dmin != NULL) {
    free(cm->dmin);
    cm->dmin = NULL;
  }
  if(cm->dmax != NULL) {
    free(cm->dmax);
    cm->dmax = NULL;
  }
  /*debug_print_cm_params(cm);*/
  while(!(BandCalculationEngine(cm, safe_windowlen, cm->beta, 0, &(cm->dmin), &(cm->dmax), NULL, NULL)))
    {
      free(cm->dmin);
      free(cm->dmax);
      cm->dmin = NULL;
      cm->dmax = NULL;
      safe_windowlen *= 2;
      if(safe_windowlen > (cm->clen * 1000))
	cm_Fail("ERROR safe_windowlen big: %d\n", safe_windowlen);
    }
  /* Set W as dmax[0], we're wasting time otherwise, looking at
   * hits that are bigger than we're allowing with QDB. */
  cm->W = cm->dmax[0];
  cm->flags |= CMH_QDB; /* raise the QDB flag */

  /* free and rebuild scan matrix to correspond to new QDBs, if it exists */
  if(cm->flags & CMH_SCANMATRIX) {
    int do_float = cm->smx->flags & cmSMX_HAS_FLOAT;
    int do_int   = cm->smx->flags & cmSMX_HAS_INT;
    cm_FreeScanMatrixForCM(cm);
    cm_CreateScanMatrixForCM(cm, do_float, do_int);
  }

  CMLogoddsify(cm); /* QDB calculation invalidates log odds scores */
  if(cm->config_opts & CM_CONFIG_ZEROINSERTS)
    CMHackInsertScores(cm);	    /* insert emissions are all equiprobable,
				     * makes all CP9 (if non-null) inserts equiprobable */
  return eslOK;
}


/* Function:  CMHackInsertScores()
 * Incept:    SRE, Wed Jul 24 09:48:22 2002 [St. Louis]
 *
 * Purpose:   Temporary (I hope): make all insert scores 0.
 *            If you let inserts train on the data, you can get
 *            positive insert emission scores. Local alignments,
 *            in particular, can then consist of just a couple of
 *            consensus states and a long string of insert 
 *            states, hitting base-composition-biased sequence
 *            with very high score. This is a Bad Thing.
 *            
 *            The long term solution for this problem will
 *            go in with mixture Dirichlet priors, but for now
 *            (with only Laplace coded), this'll appease the
 *            pitchfork and torches mob at Cambridge.
 *
 * Args:      cm - the model 
 *
 * Returns:   (void)
 *
 * Xref:      STL6 p.93.
 */
void
CMHackInsertScores(CM_t *cm)
{
  int v, x;
  for (v = 0; v < cm->M; v++)
    {
      if (cm->sttype[v] == IL_st || cm->sttype[v] == IR_st)
	for (x = 0; x < cm->abc->K; x++)
	  {
	    cm->esc[v][x]  = 0.;
	    cm->iesc[v][x] = 0;
	  }
    }
  if(cm->ioesc != NULL) { 
    for (v = 0; v < cm->M; v++)
      {
	if (cm->sttype[v] == IL_st || cm->sttype[v] == IR_st) { 
	  for (x = 0; x < cm->abc->K; x++)
	    cm->ioesc[v][x]  = 0;
	  for(x = cm->abc->K+1; x < cm->abc->Kp-1; x++) /* note boundary conditions, gap, missing data symbols stay IMPOSSIBLE */
	    cm->ioesc[v][x]  = 0;
	}
      }
  }
  if(cm->oesc != NULL) { 
    for (v = 0; v < cm->M; v++)
      {
	if (cm->sttype[v] == IL_st || cm->sttype[v] == IR_st) {
	  for (x = 0; x < cm->abc->K; x++)
	    cm->oesc[v][x]  = 0.;
	  for(x = cm->abc->K+1; x < cm->abc->Kp-1; x++) /* note boundary conditions, gap, missing data symbols stay IMPOSSIBLE */
	    cm->oesc[v][x]  = 0.;
	}
      }
  }

  if(cm->cp9 != NULL)
    CP9HackInsertScores(cm->cp9);
}

/* Function:  CP9HackInsertScores()
 * Incept:    EPN, Fri Feb  9 10:59:12 2007
 *
 * Purpose:   Make all inserts 0. Usually called from CMHackInsertScores()
 *            to make the HMM inserts match the CM inserts.
 *
 * Args:      cp9 - the CP9 HMM 
 *
 * Returns:   (void)
 */
void
CP9HackInsertScores(CP9_t *cp9)
{
  int k, x;
  for (k = 0; k <= cp9->M; k++)
    /* CP9 HMMs have insert states in nodes 0 and M */
    for (x = 0; x < MAXDEGEN; x++)
      cp9->isc[x][k] = 0;
}
