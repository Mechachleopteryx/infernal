/* Infernal's accelerated seq/profile comparison pipeline
 *  
 * Contents:
 *   1. CM_PIPELINE: allocation, initialization, destruction
 *   2. Pipeline API
 *   3. Example 1: search mode (in a sequence db)
 *   TODO: 4. Example 2: scan mode (in an HMM db)
 *   5. Copyright and license information
 * 
 * SRE, Fri Dec  5 10:09:39 2008 [Janelia] [BSG3, Bear McCreary]
 * SVN $Id: p7_pipeline.c 3352 2010-08-24 20:56:01Z eddys $
 */
#include "esl_config.h"
#include "p7_config.h"
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h> 

#include "easel.h"
#include "esl_exponential.h"
#include "esl_getopts.h"
#include "esl_gumbel.h"
#include "esl_vectorops.h"

#include "hmmer.h"
#include "funcs.h"
#include "structs.h"

/*****************************************************************
 * 1. The CM_PIPELINE object: allocation, initialization, destruction.
 *****************************************************************/

/* Function:  cm_pipeline_Create()
 * Synopsis:  Create a new accelerated comparison pipeline.
 * Incept:    EPN, Fri Sep 24 16:14:39 2010
 *            SRE, Fri Dec  5 10:11:31 2008 [Janelia] (p7_pipeline_Create())
 *
 * Purpose:   Given an application configuration structure <go>
 *            containing certain standardized options (described
 *            below), some initial guesses at the model size <M_hint>
 *            and sequence length <L_hint> that will be processed,
 *            and a <mode> that can be either <cm_SCAN_MODELS> or
 *            <cm_SEARCH_SEQS> depending on whether we're searching one sequence
 *            against a model database (cmscan mode) or one model
 *            against a sequence database (cmsearch mode); create new
 *            pipeline object.
 *
 *            In search mode, we would generally know the length of
 *            our query profile exactly, and would pass <cm->clen> as <M_hint>;
 *            in scan mode, we generally know the length of our query
 *            sequence exactly, and would pass <sq->n> as <L_hint>.
 *            Targets will come in various sizes as we read them,
 *            and the pipeline will resize any necessary objects as
 *            needed, so the other (unknown) length is only an
 *            initial allocation.
 *            
 *            The configuration <go> must include settings for the 
 *            following options:
 *            
 *            || option      ||            description                     || usually  ||
 *            | --noali      |  don't output alignments (smaller output)    |   FALSE   |
 *            | -E           |  report hits <= this E-value threshold       |    10.0   |
 *            | -T           |  report hits >= this bit score threshold     |    NULL   |
 *            | -Z           |  set initial hit search space size           |    NULL   |
 *            | --incE       |  include hits <= this E-value threshold      |    0.01   |
 *            | --incT       |  include hits >= this bit score threshold    |    NULL   |
 *            | --cut_ga     |  model-specific thresholding using GA        |   FALSE   |
 *            | --cut_nc     |  model-specific thresholding using NC        |   FALSE   |
 *            | --cut_tc     |  model-specific thresholding using TC        |   FALSE   |
 *            | --max        |  turn all heuristic filters off              |   FALSE   |
 *            | --F1         |  Stage 1 (MSV) thresh: promote hits P <= F1  |    0.35   |
 *            | --F2         |  Stage 2 (Vit) thresh: promote hits P <= F2  |    0.10   |
 *            | --F3         |  Stage 3 (Fwd) thresh: promote hits P <= F3  |    0.02   |
 *            | --nobias     |  turn OFF composition bias filter HMM        |   FALSE   |
 *            | --nonull2    |  turn OFF biased comp score correction       |    TRUE   |
 *            | --seed       |  RNG seed (0=use arbitrary seed)             |      42   |
 *            | --acc        |  prefer accessions over names in output      |   FALSE   |
 * *** opts below this line are unique to cm_pipeline.c ***
 *            | --nonull3    |  turn off NULL3 correction                   |   FALSE   |
 *            | -g           |  configure the CM for glocal alignment       |   FALSE   |
 *            | --dF3        |  Stage 3 (Fwd) per-domain thresh             |   0.01    |
 *            | --F4         |  Stage 4 (CYK) thresh: promote hits P <= F4  |   1e-4    |
 *            | --E4         |  Stage 4 (CYK) thres: promote hits E <= E4   |   NULL    |
 *            | --fast       |  set filters at strict-level                 |   FALSE   |
 *            | --mid        |  set filters at mid-level                    |   FALSE   |
 *            | --cyk        |  set final search stage as CYK, not inside   |   FALSE   |
 *            | --hmm        |  don't use CM at all, HMM only               |   FALSE   |
 *            | --noddef     |  don't domainize windows before CM stages    |   FALSE   |
 *            | --nomsv      |  skip MSV filter stage                       |   FALSE   |
 *            | --novit      |  skip Viterbi filter stage                   |   FALSE   |
 *            | --nofwd      |  skip Forward filter stage                   |   FALSE   |
 *            | --nohmm      |  skip all HMM filter stages                  |   FALSE   |
 *            | --nocyk      |  don't filter with CYK, skip to final stage  |   FALSE   |
 *            | --fnoqdb     |  don't use QDBs in CYK filter                |   FALSE   |
 *            | --fbeta      |  set beta for QDBs in CYK filter to this     |   FALSE   |
 *            | --fhbanded   |  use HMM bands for CYK filter                |   FALSE   |
 *            | --faln2bands |  get CYK filter HMM bands by aligning        |   FALSE   |
 *            | --fsums      |  use sums to get CYK filter HMM bands        |   FALSE   |
 *            | --ftau       |  set tau for HMM bands in CYK filter to this |   FALSE   |
 *            | --noqdb      |  don't use QDBs in final stage               |   FALSE   |
 *            | --beta       |  set beta for QDBs in final stage            |   FALSE   |
 *            | --hbanded    |  use HMM bands for final stage               |   FALSE   |
 *            | --aln2bands  |  get final stage HMM bands by aligning       |   FALSE   |
 *            | --sums       |  use sums to get final HMM bands             |   FALSE   |
 *            | --tau        |  set tau for final HMM bands to this         |   FALSE   |
 *            | --rt1        |  set domain def rt1 parameter as <x>         |   0.25    |
 *            | --rt2        |  set domain def rt2 parameter as <x>         |   0.10    |
 *            | --rt3        |  set domain def rt3 parameter as <x>         |   0.20    |
 *            | --skipbig    |  skip large domains > W residues             |   FALSE   |
 *            | --skipweak   |  skip low-scoring domains                    |   FALSE   |
 *            | --localdom   |  define domains in local mode                |   FALSE   |
 *            | --ns         |  set number of samples for domain traceback  |   1000    |
 * Returns:   ptr to new <cm_PIPELINE> object on success. Caller frees this
 *            with <cm_pipeline_Destroy()>.
 *
 * Throws:    <NULL> on allocation failure.
 */
CM_PIPELINE *
cm_pipeline_Create(ESL_GETOPTS *go, int clen_hint, int L_hint, enum cm_pipemodes_e mode)
{
  CM_PIPELINE *pli  = NULL;
  int          seed = esl_opt_GetInteger(go, "--seed");
  int          status;

  ESL_ALLOC(pli, sizeof(CM_PIPELINE));

  if ((pli->fwd  = p7_omx_Create(clen_hint, L_hint, L_hint)) == NULL) goto ERROR;
  if ((pli->bck  = p7_omx_Create(clen_hint, L_hint, L_hint)) == NULL) goto ERROR;
  if ((pli->oxf  = p7_omx_Create(clen_hint, 0,      L_hint)) == NULL) goto ERROR;
  if ((pli->oxb  = p7_omx_Create(clen_hint, 0,      L_hint)) == NULL) goto ERROR;     
  if ((pli->gfwd = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;
  if ((pli->gbck = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;
  if ((pli->gxf  = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;
  if ((pli->gxb  = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;     
  pli->fsmx = NULL; /* can't allocate this until we know dmin/dmax/W etc. */
  pli->smx  = NULL; /* can't allocate this until we know dmin/dmax/W etc. */

  /* Normally, we reinitialize the RNG to the original seed every time we're
   * about to collect a stochastic trace ensemble. This eliminates run-to-run
   * variability. As a special case, if seed==0, we choose an arbitrary one-time 
   * seed: time() sets the seed, and we turn off the reinitialization.
   */
  pli->r                  =  esl_randomness_CreateFast(seed);
  pli->do_reseeding       = (seed == 0) ? FALSE : TRUE;
  pli->ddef               = p7_domaindef_Create(pli->r);
  pli->ddef->do_reseeding = pli->do_reseeding;

  /* Configure reporting thresholds */
  pli->by_E            = TRUE;
  pli->E               = esl_opt_GetReal(go, "-E");
  pli->T               = 0.0;
  pli->use_bit_cutoffs = FALSE;
  if (esl_opt_IsOn(go, "-T")) 
    {
      pli->T    = esl_opt_GetReal(go, "-T"); 
      pli->by_E = FALSE;
    } 

  /* Configure inclusion thresholds */
  pli->inc_by_E           = TRUE;
  pli->incE               = esl_opt_GetReal(go, "--incE");
  pli->incT               = 0.0;
  if (esl_opt_IsOn(go, "--incT")) 
    {
      pli->incT     = esl_opt_GetReal(go, "--incT"); 
      pli->inc_by_E = FALSE;
    } 

  /* Configure for one of the model-specific thresholding options */
  if (esl_opt_GetBoolean(go, "--cut_ga"))
    {
      pli->T        = 0.0;
      pli->by_E     = FALSE;
      pli->incT     = 0.0;
      pli->inc_by_E = FALSE;
      pli->use_bit_cutoffs = CMH_GA;
    }
  if (esl_opt_GetBoolean(go, "--cut_nc"))
    {
      pli->T        = 0.0;
      pli->by_E     = FALSE;
      pli->incT     = 0.0;
      pli->inc_by_E = FALSE;
      pli->use_bit_cutoffs = CMH_NC;
    }
  if (esl_opt_GetBoolean(go, "--cut_tc"))
    {
      pli->T        = 0.0;
      pli->by_E     = FALSE;
      pli->incT     = 0.0;
      pli->inc_by_E = FALSE;
      pli->use_bit_cutoffs = CMH_TC;
    }

  /* Configure search space sizes for E value calculations  */
  pli->Z       = 0.0;
  pli->Z_setby = CM_ZSETBY_DBSIZE;
  if (esl_opt_IsOn(go, "-Z")) 
    {
      pli->Z_setby = CM_ZSETBY_OPTION;
      pli->Z       = esl_opt_GetReal(go, "-Z");
    }

  /* Configure domain definition parameters */
  pli->rt1 = esl_opt_GetReal(go, "--rt1");
  pli->rt2 = esl_opt_GetReal(go, "--rt2");
  pli->rt3 = esl_opt_GetReal(go, "--rt3");
  pli->ns  = esl_opt_GetInteger(go, "--ns");
  pli->ddef->rt1 = pli->rt1;
  pli->ddef->rt2 = pli->rt2;
  pli->ddef->rt3 = pli->rt3;
  pli->ddef->nsamples = pli->ns;
  pli->do_skipbigdoms  = esl_opt_GetBoolean(go, "--skipbig");
  pli->do_skipweakdoms = esl_opt_GetBoolean(go, "--skipweak");
  pli->do_localdoms    = (esl_opt_GetBoolean(go, "--glocaldom")) ? FALSE : TRUE;

  /* Configure acceleration pipeline thresholds */
  pli->do_cm         = TRUE;
  pli->do_hmm        = TRUE;
  pli->do_max        = FALSE;
  pli->do_mid        = FALSE;
  pli->do_fast       = FALSE;
  pli->do_domainize  = TRUE;
  pli->do_pad        = TRUE;
  pli->do_msv        = TRUE;
  pli->do_biasfilter = TRUE;
  pli->do_vit        = TRUE;
  pli->do_fwd        = TRUE;
  pli->do_cyk        = TRUE;
  pli->do_null2      = TRUE;
  pli->do_null3      = TRUE;
  pli->F1     = ESL_MIN(1.0, esl_opt_GetReal(go, "--F1"));
  pli->F2     = ESL_MIN(1.0, esl_opt_GetReal(go, "--F2"));
  pli->F3     = ESL_MIN(1.0, esl_opt_GetReal(go, "--F3"));
  pli->dF3    = ESL_MIN(1.0, esl_opt_GetReal(go, "--dF3"));
  pli->F4     = ESL_MIN(1.0, esl_opt_GetReal(go, "--F4"));
  pli->E4     = 1.;   
  pli->use_E4 = FALSE;
  if (esl_opt_IsOn(go, "--E4")) { 
    pli->E4 = esl_opt_GetReal(go, "--E4");
    pli->use_E4 = TRUE; 
  }
  if(esl_opt_GetBoolean(go, "--nomsv"))    pli->do_msv        = FALSE; 
  if(esl_opt_GetBoolean(go, "--nobias"))   pli->do_biasfilter = FALSE;
  if(esl_opt_GetBoolean(go, "--novit"))    pli->do_vit        = FALSE; 
  if(esl_opt_GetBoolean(go, "--nofwd"))    pli->do_fwd        = FALSE; 
  if(esl_opt_GetBoolean(go, "--nocyk"))    pli->do_cyk        = FALSE; 
  if(esl_opt_GetBoolean(go, "--noddef"))   pli->do_domainize  = FALSE; 
  if(esl_opt_GetBoolean(go, "--nopad"))    pli->do_pad        = FALSE;
  if(esl_opt_GetBoolean(go, "--hmm")) { 
    pli->do_cm  = FALSE;
    pli->do_cyk = FALSE; 
  }
  if(esl_opt_GetBoolean(go, "--nohmm")) { 
    pli->do_hmm        = FALSE; 
    pli->do_biasfilter = FALSE;
    pli->do_msv        = FALSE; 
    pli->do_vit        = FALSE;
    pli->do_fwd        = FALSE; 
    pli->do_domainize  = FALSE; 
  }
  if(esl_opt_GetBoolean(go, "--max")) { /* turn off all filters */
    pli->do_max = TRUE;
    pli->do_hmm = pli->do_biasfilter = pli->do_msv = pli->do_vit = pli->do_fwd = pli->do_cyk = FALSE; 
    pli->F1 = pli->F2 = pli->F3 = pli->dF3 = pli->F4 = 1.0;
  }
  if(esl_opt_GetBoolean(go, "--mid")) { /* set up mid-level filtering */
    pli->do_mid = TRUE;
    pli->do_msv = pli->do_biasfilter = FALSE;
    pli->F2 = 0.1;
    pli->F3 = 0.05;
    pli->dF3 = 0.1;
    pli->F4 = 0.001;
  }
  if(esl_opt_GetBoolean(go, "--fast")) { /* set up strict-level filtering */
    /* these are set as nhmmer defaults for HMM filters, default for CYK */
    pli->do_fast = TRUE;
    pli->F1 = 0.02;
    pli->F2 = 0.001;
    pli->F3 = 0.00001;
    pli->dF3 = 0.0001;
  }

  if (esl_opt_GetBoolean(go, "--nonull2")) pli->do_null2      = FALSE;
  if (esl_opt_GetBoolean(go, "--nonull3")) pli->do_null3      = FALSE;

  /* Configure options for the CM stages */
  pli->fcyk_cm_search_opts  = 0;
  pli->final_cm_search_opts = 0;
  pli->fcyk_beta = esl_opt_GetReal(go, "--fbeta");
  pli->fcyk_tau  = esl_opt_GetReal(go, "--ftau");
  pli->final_beta = esl_opt_GetReal(go, "--beta");
  pli->final_tau  = esl_opt_GetReal(go, "--tau");

  if(! esl_opt_GetBoolean(go, "--hmm")) { 
    /* set up filter round parameters */

    if(  esl_opt_GetBoolean(go, "--fnoqdb"))      pli->fcyk_cm_search_opts  |= CM_SEARCH_NOQDB;
    if(  esl_opt_GetBoolean(go, "--fhbanded"))    pli->fcyk_cm_search_opts  |= CM_SEARCH_HBANDED;
    if(  esl_opt_GetBoolean(go, "--faln2bands"))  pli->fcyk_cm_search_opts  |= CM_SEARCH_HMMALNBANDS;
    if(  esl_opt_GetBoolean(go, "--fsums"))       pli->fcyk_cm_search_opts  |= CM_SEARCH_SUMS;
    if(! esl_opt_GetBoolean(go, "--nonull3"))     pli->fcyk_cm_search_opts  |= CM_SEARCH_NULL3;

    /* set up final round parameters */
    if(! esl_opt_GetBoolean(go, "--cyk"))         pli->final_cm_search_opts |= CM_SEARCH_INSIDE;
    if(  esl_opt_GetBoolean(go, "--noqdb"))       pli->final_cm_search_opts |= CM_SEARCH_NOQDB;
    if(  esl_opt_GetBoolean(go, "--hbanded"))     pli->final_cm_search_opts |= CM_SEARCH_HBANDED;
    if(  esl_opt_GetBoolean(go, "--aln2bands"))   pli->final_cm_search_opts |= CM_SEARCH_HMMALNBANDS;
    if(  esl_opt_GetBoolean(go, "--sums"))        pli->final_cm_search_opts |= CM_SEARCH_SUMS;
    if(! esl_opt_GetBoolean(go, "--nonull3"))     pli->final_cm_search_opts |= CM_SEARCH_NULL3;
  }

  /* Determine statistics modes for CM stages */
  pli->fcyk_cm_exp_mode  = (esl_opt_GetBoolean(go, "-g")) ? EXP_CM_GC : EXP_CM_LC;
  if(pli->final_cm_search_opts & CM_SEARCH_INSIDE) { 
    pli->final_cm_exp_mode = (esl_opt_GetBoolean(go, "-g")) ? EXP_CM_GI : EXP_CM_LI;
  }
  else {
    pli->final_cm_exp_mode = (esl_opt_GetBoolean(go, "-g")) ? EXP_CM_GC : EXP_CM_LC;
  }

  /* Accounting as we collect results */
  pli->nmodels         = 0;
  pli->nseqs           = 0;
  pli->nres            = 0;
  pli->nnodes          = 0;
  pli->n_past_msv      = 0;
  pli->n_past_bias     = 0;
  pli->n_past_vit      = 0;
  pli->n_past_fwd      = 0;
  pli->n_past_cyk      = 0;
  pli->n_past_ins      = 0;
  pli->pos_past_msv    = 0;
  pli->pos_past_bias   = 0;
  pli->pos_past_vit    = 0;
  pli->pos_past_fwd    = 0;
  pli->pos_past_cyk    = 0;
  pli->pos_past_ins    = 0;
  pli->mode            = mode;
  pli->show_accessions = (esl_opt_GetBoolean(go, "--acc")   ? TRUE  : FALSE);
  pli->show_alignments = (esl_opt_GetBoolean(go, "--noali") ? FALSE : TRUE);
  pli->cmfp            = NULL;
  pli->errbuf[0]       = '\0';

  /* EPN TEMP: remove once I get a data structure that holds CM parsetrees */
  pli->show_alignments = FALSE;
  return pli;

 ERROR:
  cm_pipeline_Destroy(pli);
  return NULL;
}


/* Function:  cm_pipeline_Reuse()
 * Synopsis:  Reuse a pipeline for next target.
 * Incept:    EPN, Fri Sep 24 16:28:55 2010
 *            SRE, Fri Dec  5 10:31:36 2008 [Janelia] (p7_pipeline_Reuse())
 *
 * Purpose:   Reuse <pli> for next target sequence (search mode)
 *            or model (scan mode). 
 *            
 *            May eventually need to distinguish from reusing pipeline
 *            for next query, but we're not really focused on multiquery
 *            use of hmmscan/hmmsearch/phmmer for the moment.
 */
int
cm_pipeline_Reuse(CM_PIPELINE *pli)
{
  p7_omx_Reuse(pli->oxf);
  p7_omx_Reuse(pli->oxb);
  p7_omx_Reuse(pli->fwd);
  p7_omx_Reuse(pli->bck);
  p7_domaindef_Reuse(pli->ddef);
  /* TO DO, write scanmatrixreuse */
  return eslOK;
}


/* Function:  cm_pipeline_Destroy()
 * Synopsis:  Free a <CM_PIPELINE> object.
 * Incept:    EPN, Fri Sep 24 16:29:34 2010
 *            SRE, Fri Dec  5 10:30:23 2008 [Janelia] (p7_pipeline_Destroy())
 *
 * Purpose:   Free a <CM_PIPELINE> object.
 */
void
cm_pipeline_Destroy(CM_PIPELINE *pli)
{
  if (pli == NULL) return;
  
  p7_omx_Destroy(pli->oxf);
  p7_omx_Destroy(pli->oxb);
  p7_omx_Destroy(pli->fwd);
  p7_omx_Destroy(pli->bck);
  esl_randomness_Destroy(pli->r);
  p7_domaindef_Destroy(pli->ddef);
  free(pli);
}
/*---------------- end, CM_PIPELINE object ----------------------*/


/*****************************************************************
 * 2. The pipeline API.
 *****************************************************************/

/* Function:  cm_pli_TargetReportable
 * Synopsis:  Returns TRUE if target score meets reporting threshold.
 * Incept:    EPN, Fri Sep 24 16:34:37 2010
 *            SRE, Tue Dec  9 08:57:26 2008 [Janelia] (p7_pli_TargetReportable())
 *
 * Purpose:   Returns <TRUE> if the bit score <score> and/or 
 *            E-value <Eval> meets per-target reporting thresholds 
 *            for the processing pipeline.
 */
int
cm_pli_TargetReportable(CM_PIPELINE *pli, float score, double Eval)
{
  if      (  pli->by_E && Eval  <= pli->E) return TRUE;
  else if (! pli->by_E && score >= pli->T) return TRUE;
  return FALSE;
}

/* Function:  cm_pli_TargetIncludable()
 * Synopsis:  Returns TRUE if target score meets inclusion threshold.
 * Incept:    EPN, Fri Sep 24 16:32:43 2010
 *            SRE, Fri Jan 16 11:18:08 2009 [Janelia] (p7_pli_TargetIncludable())
 */
int
cm_pli_TargetIncludable(CM_PIPELINE *pli, float score, double Eval)
{
  if      (  pli->by_E && Eval  <= pli->incE) return TRUE;
  else if (! pli->by_E && score >= pli->incT) return TRUE;

  return FALSE;
}

/* Function:  cm_pli_NewModel()
 * Synopsis:  Prepare pipeline for a new CM/HMM
 * Incept:    EPN, Fri Sep 24 16:35:35 2010
 *            SRE, Fri Dec  5 10:35:37 2008 [Janelia] (p7_pli_NewModel())
 *
 * Purpose:   Caller has a new model <cm>, and optimized HMM matrix <om>. 
 *            Prepare the pipeline <pli> to receive this model as either 
 *            a query or a target.
 *
 *            The pipeline may alter the null model <bg> in a model-specific
 *            way (if we're using a composition bias filter HMM in the
 *            pipeline).
 *
 * Returns:   <eslOK> on success.
 * 
 *            <eslEINVAL> if pipeline expects to be able to use a
 *            model's bit score thresholds, but this model does not
 *            have the appropriate ones set.
 */
int
cm_pli_NewModel(CM_PIPELINE *pli, CM_t *cm, int *fcyk_dmin, int *fcyk_dmax, int *final_dmin, int *final_dmax, const P7_OPROFILE *om, P7_BG *bg)
{
  int status = eslOK;

  pli->nmodels++;
  pli->nnodes += om->M;

  pli->fsmx = cm_CreateScanMatrix(cm, cm->W, fcyk_dmin, fcyk_dmax, 
				  ((fcyk_dmin == NULL && fcyk_dmax == NULL) ? cm->W : fcyk_dmax[0]),
				  pli->fcyk_beta, 
				  ((fcyk_dmin == NULL && fcyk_dmax == NULL) ? FALSE : TRUE),
				  TRUE,    /* do     allocate float matrices for CYK filter round */
				  FALSE);  /* do not allocate int   matrices for CYK filter round  */

  pli->smx  = cm_CreateScanMatrix(cm, cm->W, final_dmin, final_dmax, 
				  ((final_dmin == NULL && final_dmax == NULL) ? cm->W : final_dmax[0]),
				  pli->final_beta, 
				  ((final_dmin == NULL && final_dmax == NULL) ? FALSE : TRUE),
		 		  FALSE,  /* do not allocate float matrices for Inside round */
			 	  TRUE);  /* do     allocate int   matrices for Inside round */

  
  if (pli->Z_setby == p7_ZSETBY_NTARGETS && pli->mode == p7_SCAN_MODELS) pli->Z = pli->nmodels;

  if (pli->do_biasfilter) p7_bg_SetFilter(bg, om->M, om->compo);

  pli->W = cm->W;

  return status;
}


/* Function:  cm_pli_NewSeq()
 * Synopsis:  Prepare pipeline for a new sequence (target or query)
 * Incept:    EPN, Fri Sep 24 16:39:52 2010
 *            SRE, Fri Dec  5 10:57:15 2008 [Janelia] (p7_pli_NewSeq())
 *
 * Purpose:   Caller has a new sequence <sq>. Prepare the pipeline <pli>
 *            to receive this model as either a query or a target.
 *
 * Returns:   <eslOK> on success.
 */
int
cm_pli_NewSeq(CM_PIPELINE *pli, CM_t *cm, const ESL_SQ *sq)
{
  int status;
  float T;
  pli->nres += sq->n;
  if (pli->Z_setby == CM_ZSETBY_DBSIZE && pli->mode == CM_SEARCH_SEQS) pli->Z = pli->nres;

  if((status = UpdateExpsForDBSize(cm, NULL, (long) pli->nres))          != eslOK) return status;
  if((status = E2MinScore(cm, NULL, pli->final_cm_exp_mode, pli->E, &T)) != eslOK) return status;
  pli->T = (double) T;

  return eslOK;
}

/* Function:  cm_pipeline_Merge()
 * Synopsis:  Merge the pipeline statistics
 * Incept:    EPN, Fri Sep 24 16:41:17 2010   
 *
 * Purpose:   Caller has a new model <om>. Prepare the pipeline <pli>
 *            to receive this model as either a query or a target.
 *
 *            The pipeline may alter the null model <bg> in a model-specific
 *            way (if we're using a composition bias filter HMM in the
 *            pipeline).
 *
 * Returns:   <eslOK> on success.
 * 
 *            <eslEINVAL> if pipeline expects to be able to use a
 *            model's bit score thresholds, but this model does not
 *            have the appropriate ones set.
 */
int
cm_pipeline_Merge(CM_PIPELINE *p1, CM_PIPELINE *p2)
{
  /* if we are searching a sequence database, we need to keep track of the
   * number of sequences and residues processed.
   */
  if (p1->mode == CM_SEARCH_SEQS)
    {
      p1->nseqs   += p2->nseqs;
      p1->nres    += p2->nres;
    }
  else
    {
      p1->nmodels += p2->nmodels;
      p1->nnodes  += p2->nnodes;
    }

  p1->n_past_msv  += p2->n_past_msv;
  p1->n_past_bias += p2->n_past_bias;
  p1->n_past_vit  += p2->n_past_vit;
  p1->n_past_fwd  += p2->n_past_fwd;
  p1->n_output    += p2->n_output;

  p1->pos_past_msv  += p2->pos_past_msv;
  p1->pos_past_bias += p2->pos_past_bias;
  p1->pos_past_vit  += p2->pos_past_vit;
  p1->pos_past_fwd  += p2->pos_past_fwd;
  p1->pos_output    += p2->pos_output;

  if (p1->Z_setby == p7_ZSETBY_NTARGETS)
    {
      p1->Z += (p1->mode == p7_SCAN_MODELS) ? p2->nmodels : p2->nseqs;
    }
  else
    {
      p1->Z = p2->Z;
    }

  return eslOK;
}

/* Function:  cm_Pipeline()
 * Synopsis:  The accelerated seq/profile comparison pipeline using HMMER3 scanning 
 * Incept:    EPN, Fri Sep 24 16:42:21 2010
 *            TJW, Fri Feb 26 10:17:53 2018 [Janelia] (p7_Pipeline_Longtargets())
 *
 * Purpose:   Run the accelerated pipeline to compare profile <om>
 *            against sequence <sq>. If a significant hit is found,
 *            information about it is added to the <hitlist>.
 *            This is a variant of p7_Pipeline that runs the
 *            versions of the MSV/SSV filters that scan a long
 *            sequence and find high-scoring regions (windows), then pass 
 *            those to the remainder of the pipeline. The pipeline
 *            accumulates beancounting information about how many comparisons
 *            flow through the pipeline while it's active.
 *
 * Returns:   <eslOK> on success. If a significant hit is obtained,
 *            its information is added to the growing <hitlist>.
 *
 *            <eslEINVAL> if (in a scan pipeline) we're supposed to
 *            set GA/TC/NC bit score thresholds but the model doesn't
 *            have any.
 *
 *            <eslERANGE> on numerical overflow errors in the
 *            optimized vector implementations; particularly in
 *            posterior decoding. I don't believe this is possible for
 *            multihit local models, but I'm set up to catch it
 *            anyway. We may emit a warning to the user, but cleanly
 *            skip the problematic sequence and continue.
 *
 * Throws:    <eslEMEM> on allocation failure.
 *
 * Xref:      J4/25.
 */
int
cm_Pipeline(CM_PIPELINE *pli, CM_t *cm, P7_OPROFILE *om, P7_PROFILE *gm, P7_BG *bg, const ESL_SQ *sq, P7_TOPHITS *hitlist)
{
  P7_HIT          *hit     = NULL;     /* ptr to the current hit output data      */
  float            usc, vfsc, fwdsc, cyksc, inssc; /* filter scores                           */
  float            filtersc;           /* HMM null filter score                   */
  float            nullsc;             /* null model score                        */
  float            seq_score;          /* the corrected per-seq bit score */
  double           P;                  /* P-value of a hit */
  double           E;                  /* E-value of a hit */
  int              d, i, h;
  int64_t         *dstarts, *dends;    /* boundaries of 'domains' */
  int              ndom;               /* number of domains */
  int              ndom_alloc;         /* current size of dstarts, dends */
  int              status;
  char             errbuf[cmERRBUFSIZE];
  void            *p;                 /* for ESL_RALLOC */
  search_results_t *results;
  int              ali_len, env_len, dom_wlen; /* lengths of alignment, envelope, domain window */
  float            dom_sc, dom_nullsc; /* domain bit score, and domain null1 score */

  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */
  p7_omx_GrowTo(pli->oxf, om->M, 0, sq->n);    /* expand the one-row omx if needed */

  /* Set false target length. This is a conservative estimate of the length of window that'll
   * soon be passed on to later phases of the pipeline;  used to recover some bits of the score
   * that we would miss if we left length parameters set to the full target length */
  p7_oprofile_ReconfigMSVLength(om, om->max_length);

  /* First level filter: the MSV filter, multihit with <om>.
   * This variant of MSV will scan a long sequence and find
   * short high-scoring regions.
   * */
  int* window_starts;
  int* window_ends;
  int hit_cnt;

  ndom_alloc = 10;
  ESL_ALLOC(dstarts, sizeof(int64_t) * ndom_alloc);
  ESL_ALLOC(dends,   sizeof(int64_t) * ndom_alloc);

  printf("sq: %s\nsq->n: %" PRId64 " cm->W:  %d  pli->W: %d\n", sq->name, sq->n, cm->W, pli->W);
  /*********************************************/
  /* Filter 1: MSV with p7 HMM */
  if(pli->do_msv) { 
    p7_MSVFilter_longtarget(sq->dsq, sq->n, om, pli->oxf, bg, pli->F1, &window_starts, &window_ends, &hit_cnt);

    if (hit_cnt == 0 ) return eslOK;
  }
  else { /* all windows automatically pass MSV, divide up into windows of 2*W, overlapping by W-1 residues */
    hit_cnt = 1; /* first window */
    if(sq->n > (2 * pli->W)) { 
      hit_cnt += (int) (sq->n - (2 * pli->W)) / ((2 * pli->W) - (pli->W - 1));
      /*               (L     -  first window)/(number of unique residues per window) */
      if(((sq->n - (2 * pli->W)) % ((2 * pli->W) - (pli->W - 1))) > 0) { 
	hit_cnt++; /* if the (int) cast in previous line removed any fraction of a window, we add it back here */
      }
    }
    ESL_ALLOC(window_starts, sizeof(int) * hit_cnt);
    ESL_ALLOC(window_ends,   sizeof(int) * hit_cnt);
    for(i = 0; i < hit_cnt; i++) { 
      window_starts[i] = 1 + (i * (pli->W + 1));
      window_ends[i]   = ESL_MIN((window_starts[i] + (2*pli->W) - 1), sq->n);
      /*printf("window %5d/%5d  %10d..%10d (L=%10" PRId64 ")\n", i+1, hit_cnt, window_starts[i], window_ends[i], sq->n);*/
    }
  }      
  pli->n_past_msv += hit_cnt;
    
  /*********************************************/
  ESL_SQ *tmpseq = esl_sq_CreateDigital(sq->abc);
  ESL_DSQ* subseq;

  for (i=0; i<hit_cnt; i++) {
    printf("\n\nWindow %5d [%10d..%10d] survived MSV.\n", i, window_starts[i], window_ends[i]);
    int window_len = window_ends[i] - window_starts[i] + 1;
    pli->pos_past_msv += window_len;
    
    subseq = sq->dsq + window_starts[i] - 1;
    p7_bg_SetLength(bg, window_len);
    p7_bg_NullOne  (bg, subseq, window_len, &nullsc);
    
    if (pli->do_biasfilter) {
      /******************************************************************************/
      /* Filter 1B: Bias filter with p7 HMM */
      /* Have to run msv again, to get the full score for the window.
	 (using the standard "per-sequence" msv filter this time). */
      p7_oprofile_ReconfigMSVLength(om, window_len);
      p7_MSVFilter(subseq, window_len, om, pli->oxf, &usc);
      p7_bg_FilterScore(bg, subseq, window_len, &filtersc);
      
      seq_score = (usc - filtersc) / eslCONST_LOG2;
      P = esl_gumbel_surv(seq_score,  om->evparam[p7_MMU],  om->evparam[p7_MLAMBDA]);
      
      if (P > pli->F1) continue;
      /******************************************************************************/
    }
    else {
      filtersc = nullsc;
      P = 1; /* to ensure that ViterbiFilter gets run */
    }
    pli->n_past_bias++;
    pli->pos_past_bias += window_len;
      
    p7_oprofile_ReconfigRestLength(om, window_len);
    
    printf("Window %5d [%10d..%10d] survived Bias.\n", i, window_starts[i], window_ends[i]);

    if (pli->do_vit && P > pli->F2) { 
      /******************************************************************************/
      /* Filter 2: Viterbi with p7 HMM */
      /* Second level filter: ViterbiFilter(), multihit with <om> */
      p7_ViterbiFilter(subseq, window_len, om, pli->oxf, &vfsc);
      seq_score = (vfsc-filtersc) / eslCONST_LOG2;
      P  = esl_gumbel_surv(seq_score,  om->evparam[p7_VMU],  om->evparam[p7_VLAMBDA]);
      if (P > pli->F2) continue;
      /******************************************************************************/
    }
    pli->n_past_vit++;
    pli->pos_past_vit += window_len;
    
    printf("Window %5d [%10d..%10d] survived Vit.\n", i, window_starts[i], window_ends[i]);

    if(pli->do_fwd) { 
      /******************************************************************************/
      /* Filter 3: Forward with p7 HMM */
      /* Parse it with Forward and obtain its real Forward score. */
      p7_ForwardParser(subseq, window_len, om, pli->oxf, &fwdsc);
      seq_score = (fwdsc-filtersc) / eslCONST_LOG2;
      P = esl_exp_surv(seq_score,  om->evparam[p7_FTAU],  om->evparam[p7_FLAMBDA]);
      if (P > pli->F3) continue;
    }
    /******************************************************************************/
    pli->n_past_fwd++;
    pli->pos_past_fwd += window_len;

    printf("Window %5d [%10d..%10d] survived Fwd (sc: %6.2f bits;  P: %g).\n", i, window_starts[i], window_ends[i], seq_score, P);

    if(! pli->do_domainize) { 
      /* we'll pass the full window onto the next round */
      ndom = 1;
      dstarts[0] = 1;
      dends[0]   = window_len;
    }
    else { 
      /* pli->do_domainize == TRUE: define domains with HMM.
       * 
       * 2 possible scenarios:
       * 
       *   pli->do_cm == TRUE: for each domain add some adjacent
       *   nucleotides to it and pass it onto a CM search stage.
       *   
       *   pli->do_cm == FALSE: the domains are the final hits.
       */

      /* set up seq object for domaindef function */
      esl_sq_GrowTo(tmpseq, window_len);
      memcpy((void*)(tmpseq->dsq), subseq, (window_len+1) * sizeof(uint8_t) ); // len+1 to account for the 0 position plus len others
      tmpseq->dsq[window_len+1]= eslDSQ_SENTINEL;
      tmpseq->n = window_len;

      if(pli->do_localdoms) { /* we can use optimized matrices and, consequently, p7_domaindef_ByPosteriorHeuristics */
	/* Do a Forward parse, if we didn't already */
	if(! pli->do_fwd) p7_ForwardParser(tmpseq->dsq, window_len, om, pli->oxf, NULL);
	/* Now a Backwards parser pass, and hand it to domain definition workflow. */
	p7_omx_GrowTo(pli->oxb, om->M, 0, window_len);
	p7_BackwardParser(tmpseq->dsq, window_len, om, pli->oxf, pli->oxb, NULL);
      }
      else { /* we're defining domains in glocal mode, so we need to fill 
		generic fwd/bck matrices and pass them to p7_domaindef_GlocalByPosteriorHeuristics() */
	p7_gmx_GrowTo(pli->gxf, gm->M, window_len);
	p7_GForward (tmpseq->dsq, window_len, gm, pli->gxf, NULL);
	p7_gmx_GrowTo(pli->gxb, gm->M, window_len);
	p7_GBackward(tmpseq->dsq, window_len, gm, pli->gxb, NULL);
      }
      if(pli->do_localdoms) { status = p7_domaindef_ByPosteriorHeuristics      (tmpseq, om, pli->oxf, pli->oxb, pli->fwd,  pli->bck,  pli->ddef); }
      else                  { status = p7_domaindef_GlocalByPosteriorHeuristics(tmpseq, gm, pli->gxf, pli->gxb, pli->gfwd, pli->gbck, pli->ddef); }

      if (status != eslOK) ESL_FAIL(status, pli->errbuf, "domain definition workflow failure"); /* eslERANGE can happen */
      if (pli->ddef->nregions   == 0)  continue; /* score passed threshold but there's no discrete domains here       */
      if (pli->ddef->nenvelopes == 0)  continue; /* rarer: region was found, stochastic clustered, no envelopes found */
      ndom = pli->ddef->ndom;
      if(ndom > ndom_alloc) { 
	ESL_RALLOC(dstarts, p, sizeof(int64_t) * (2*ndom));
	ESL_RALLOC(dends,   p, sizeof(int64_t) * (2*ndom));
	ndom_alloc = 2*ndom;
      }
      for(d = 0; d < ndom; d++) { 
	/* Modify the bit score using Travis' corrections originally from p7_pipeline.c::p7_Pipeline_LongTarget() 
	 * (these corrections are also included below for hmmonly hit reporting) */
	env_len = pli->ddef->dcl[d].jenv - pli->ddef->dcl[d].ienv +1;
	ali_len = pli->ddef->dcl[d].jali - pli->ddef->dcl[d].iali +1;
	dom_wlen = ESL_MIN(pli->W, window_len); //see notes, ~/notebook/20100716_hmmer_score_v_eval_bug/, end of Thu Jul 22 13:36:49 EDT 2010
	dom_sc  = pli->ddef->dcl[d].envsc;
	/* For these modifications, see notes, ~/notebook/20100716_hmmer_score_v_eval_bug/, end of Thu Jul 22 13:36:49 EDT 2010 */
	dom_sc -= 2 * log(2. / (window_len+2)) +   (env_len-ali_len) * log((float) window_len / (window_len+2));
	dom_sc += 2 * log(2. / (dom_wlen+2)) ;
	/* handle extremely rare case that the env_len is actually larger than om->max_length */
	/* (I don't think its very rare for dcmsearch b/c MSV P value threshold is so high */
	dom_sc +=  (ESL_MAX(dom_wlen, env_len) - ali_len) * log((float) dom_wlen / (float) (dom_wlen+2));

	dom_nullsc = (float) dom_wlen * log((float)dom_wlen/(dom_wlen+1)) + log(1./(dom_wlen+1));
	dom_sc = (dom_sc - dom_nullsc) / eslCONST_LOG2;
	P = esl_exp_surv (dom_sc,  om->evparam[p7_FTAU], om->evparam[p7_FLAMBDA]);

	printf("\t\tdomain %5d  env bits: %6.2f (uncorrected: %6.2f) P: %g (uncorrected: %g)\n", 
	       d+1, dom_sc, 
	       pli->ddef->dcl[d].envsc / eslCONST_LOG2, 
	       P,
	       esl_exp_surv(pli->ddef->dcl[d].envsc / eslCONST_LOG2,
			    om->evparam[p7_FTAU], om->evparam[p7_FLAMBDA]));

	if(pli->do_skipweakdoms && P > pli->dF3) { 
	  dstarts[d] = dends[d] = -1; /* we won't pass this to the CM later */
	  continue;
	}
	/* define window to search with CM:
	 *                domain/hit
	 *   ----------xxxxxxxxxxxxxxxx---------
	 *   |<-----------------------|
	 *                W
	 *             |---------------------->|
	 *                         W
	 *
	 * On rare occasions, the envelope size may exceed W, in that case we
	 * define dstarts-W/2..dend+W/2 as the envelope.
	 * 
	 */
	if(pli->do_pad) { 
	  if(esl_exp_surv(pli->ddef->dcl[d].envsc / eslCONST_LOG2,
			  om->evparam[p7_FTAU], om->evparam[p7_FLAMBDA]));
	  
	  if(((pli->ddef->dcl[d].jenv - pli->ddef->dcl[d].ienv) + 1) > pli->W) { 
	    if(pli->do_skipbigdoms) { 
	      dstarts[d] = dends[d] = -1; /* we won't pass this to the CM later */
	    }
	    else { 
	      /* envelope size > W, pad W-1 residues to each side */
	      dstarts[d] = ESL_MAX(1,          pli->ddef->dcl[d].ienv - (int) ((pli->W - 1)));
	      dends[d]   = ESL_MIN(window_len, pli->ddef->dcl[d].jenv + (int) ((pli->W - 1)));
	    }
	  }
	  else { 
	    /* envelope size is less than or equal to W */
	    dstarts[d] = ESL_MAX(1,          pli->ddef->dcl[d].jenv - (pli->W-1));
	    dends[d]   = ESL_MIN(window_len, pli->ddef->dcl[d].ienv + (pli->W-1));
	  }
	}
	else { 
	  dstarts[d] = pli->ddef->dcl[d].ienv;
	  dends[d]   = pli->ddef->dcl[d].jenv;
	}
	printf("\tdefining domain %3d  [%7d..%7d] --> [%7d..%7d]\n", 
	       d, pli->ddef->dcl[d].ienv, pli->ddef->dcl[d].jenv, 
	       dstarts[d], dends[d]);
      }
    }
    if(! pli->do_cm) { 
      /* If we're only using the HMM, define the hits as the domains:
       * Much of this code/comments has been copied from
       * p7_pipeline_LongTarget().
       *
       * Put the domains into the hit list. Some of them may not
       * pass eventual E-value thresholds, so this list may be longer
       * than eventually reported.
       * 
       * Modified original pipeline to create a single hit
       * for each domain, so the remainder of the typical-case hit-merging
       * process can remain mostly intact.
       */
      for (d = 0; d < pli->ddef->ndom; d++) { 
	p7_tophits_CreateNextHit(hitlist, &hit);
	
	hit->ndom        = 1;
	hit->best_domain = 0;
	
	hit->window_length = ESL_MIN(om->max_length, window_len); //see notes, ~/notebook/20100716_hmmer_score_v_eval_bug/, end of Thu Jul 22 13:36:49 EDT 2010
	hit->subseq_start = sq->start;
	
	ESL_ALLOC(hit->dcl, sizeof(P7_DOMAIN) );
	hit->dcl[0] = pli->ddef->dcl[d];
	
	hit->dcl[0].ienv += window_starts[i] - 1; // represents the real position within the sequence handed to the pipeline
	hit->dcl[0].jenv += window_starts[i] - 1;
	hit->dcl[0].iali += window_starts[i] - 1;
	hit->dcl[0].jali += window_starts[i] - 1;
	hit->dcl[0].ad->sqfrom += window_starts[i] - 1;
	hit->dcl[0].ad->sqto += window_starts[i] - 1;
	
	printf("\tTMP domain %3d  [%7d..%7d]\n",
	       d, hit->dcl[0].iali, hit->dcl->jali);

	//adjust the score of a hit to account for the full length model - the characters ouside the envelope but in the window
	env_len = hit->dcl[0].jenv - hit->dcl[0].ienv + 1;
	ali_len = hit->dcl[0].jali - hit->dcl[0].iali + 1;
	hit->dcl[0].bitscore = hit->dcl[0].envsc ;
	//For these modifications, see notes, ~/notebook/20100716_hmmer_score_v_eval_bug/, end of Thu Jul 22 13:36:49 EDT 2010
	hit->dcl[0].bitscore -= 2 * log(2. / (window_len+2))          +   (env_len-ali_len)            * log((float)window_len / (window_len+2));
	hit->dcl[0].bitscore += 2 * log(2. / (hit->window_length+2)) ;
	//handle extremely rare case that the env_len is actually larger than om->max_length
	hit->dcl[0].bitscore +=  (ESL_MAX(hit->window_length, env_len) - ali_len) * log((float)hit->window_length / (float) (hit->window_length+2));
	
	hit->pre_score  = hit->dcl[0].bitscore  / eslCONST_LOG2;
	hit->pre_pvalue = esl_exp_surv (hit->pre_score,  om->evparam[p7_FTAU], om->evparam[p7_FLAMBDA]);
	
	float nullsc2 =  (float) hit->window_length * log((float)hit->window_length/(hit->window_length+1)) + log(1./(hit->window_length+1));
	
	hit->dcl[0].dombias  = (pli->do_null2 ? p7_FLogsum(0.0, log(bg->omega) + hit->dcl[0].domcorrection) : 0.0);
	hit->dcl[0].bitscore = (hit->dcl[0].bitscore - (nullsc2 + hit->dcl[0].dombias)) / eslCONST_LOG2;
	hit->dcl[0].pvalue   = esl_exp_surv (hit->dcl[0].bitscore,  om->evparam[p7_FTAU], om->evparam[p7_FLAMBDA]);
	
	if (pli->mode == CM_SEARCH_SEQS) { 
	  if (                       (status  = esl_strdup(sq->name, -1, &(hit->name)))  != eslOK) ESL_EXCEPTION(eslEMEM, "allocation failure");
	  if (sq->acc[0]  != '\0' && (status  = esl_strdup(sq->acc,  -1, &(hit->acc)))   != eslOK) ESL_EXCEPTION(eslEMEM, "allocation failure");
	} 
	else {
	  if ((status  = esl_strdup(om->name, -1, &(hit->name)))  != eslOK) esl_fatal("allocation failure");
	  if ((status  = esl_strdup(om->acc,  -1, &(hit->acc)))   != eslOK) esl_fatal("allocation failure");
	}
	
	hit->sum_score  = hit->score  = hit->dcl[0].bitscore;
	hit->sum_pvalue = hit->pvalue = hit->dcl[0].pvalue;
      }
      pli->ddef->ndom = 0; // reset for next use
    } /* end of if(! pli->do_cm) */
    else { 
      /* We are using the CM, search the window containing each domain 
       * (as defined above for domain d as dstarts[d]..dends[d]) 
       * with CYK and/or Inside.
       */
      for (d = 0; d < ndom; d++) { 
	if(dstarts[d] == -1) continue; /* if pli->do_skipbigdoms, we skip any domains > W, dstarts[d] (and dends[d]) were set to -1 above */

	printf("\nWindow %5d [%10d..%10d] domain %5d [%10d..%10d] being passed to CYK.\n", i, window_starts[i], window_ends[i], d, dstarts[d], dends[d]);

	if(pli->do_cyk) { 
	  /******************************************************************************/
	  /* Filter 4: CYK with CM */
	  cm->search_opts  = pli->fcyk_cm_search_opts;
	  if(pli->fcyk_cm_search_opts & CM_SEARCH_HBANDED) { /* use HMM bands */
	    if((status = cp9_Seq2Bands(cm, errbuf, cm->cp9_mx, cm->cp9_bmx, cm->cp9_bmx, subseq, dstarts[d], dends[d], cm->cp9b, TRUE, 0)) != eslOK) { 
	      printf("ERROR: %s\n", errbuf); return status; }
	    PrintDPCellsSaved_jd(cm, cm->cp9b->jmin, cm->cp9b->jmax, cm->cp9b->hdmin, cm->cp9b->hdmax, ESL_MIN(cm->W, dends[d]-dstarts[d]+1)); 
	    if((status = FastCYKScanHB(cm, errbuf, subseq, dstarts[d], dends[d], 
				       0.,            /* minimum score to report, irrelevant */
				       NULL,          /* results to add to, NULL in this case */
				       pli->do_null3, /* do the NULL3 correction? */
				       cm->hbmx,      /* the HMM banded matrix */
				       32768.,        /* upper limit for size of DP matrix, 32 Gb */
				       &cyksc)) != eslOK) { 
	      printf("ERROR: %s\n", errbuf); return status;  }
	  }
	  else { /* don't use HMM bands */
	  /*printf("Running CYK on window %d\n", i);*/
	  if((status = FastCYKScan(cm, errbuf, pli->fsmx, subseq, dstarts[d], dends[d],
				   0.,            /* minimum score to report, irrelevant */
				   NULL,          /* results to add to, NULL in this case */
				   pli->do_null3, /* do the NULL3 correction? */
				   NULL,          /* ret_vsc, irrelevant here */
				   &cyksc)) != eslOK) { 
	    printf("ERROR: %s\n", errbuf); return status;  }
	  }
	  P = esl_exp_surv(cyksc, cm->stats->expAA[pli->fcyk_cm_exp_mode][0]->mu_extrap, cm->stats->expAA[pli->fcyk_cm_exp_mode][0]->lambda);
	  E = P * cm->stats->expAA[pli->fcyk_cm_exp_mode][0]->cur_eff_dbsize;
	  printf("\t\t\tCYK      %7.2f bits  E: %g  P: %g\n", cyksc, E, P);
	  if ((!pli->use_E4) && (P > pli->F4)) continue;
	  if (( pli->use_E4) && (E > pli->E4)) continue;
	  /******************************************************************************/
	}	
	pli->n_past_cyk++;
	pli->pos_past_cyk += dends[d]-dstarts[d]+1;

	printf("Window %5d [%10d..%10d] domain %5d [%10d..%10d] being passed to Inside.\n", i, window_starts[i], window_ends[i], d, dstarts[d], dends[d]);

	/******************************************************************************/
	/* Final stage: Inside/CYK with CM, report hits to a search_results_t data structure. */
	cm->search_opts  = pli->final_cm_search_opts;
	results = CreateResults(INIT_RESULTS);

	/*******************************************************************
	 * Determine if we're doing a HMM banded scan, if so, we may already have HMM bands 
	 * if our CYK filter also used them. 
	 *******************************************************************/
	if(pli->final_cm_search_opts & CM_SEARCH_HBANDED) { /* use HMM bands */
	  if((! pli->do_cyk) || (! (pli->fcyk_cm_search_opts & CM_SEARCH_HBANDED))) { /* we need to calculate the HMM bands */
	    if((status = cp9_Seq2Bands(cm, errbuf, cm->cp9_mx, cm->cp9_bmx, cm->cp9_bmx, subseq, dstarts[d], dends[d], cm->cp9b, TRUE, 0)) != eslOK) { 
	      printf("ERROR: %s\n", errbuf); return status; }
	    PrintDPCellsSaved_jd(cm, cm->cp9b->jmin, cm->cp9b->jmax, cm->cp9b->hdmin, cm->cp9b->hdmax, ESL_MIN(cm->W, dends[d]-dstarts[d]+1)); 
	  }
	  if(cm->search_opts & CM_SEARCH_INSIDE) { /* final algorithm is HMM banded Inside */
	    /*printf("calling HMM banded Inside scan\n");*/
	    if((status = FastFInsideScanHB(cm, errbuf, subseq, dstarts[d], dends[d], 
					   pli->T,            /* minimum score to report */
					   results,           /* our results data structure that will store hit(s) */
					   pli->do_null3,     /* do the NULL3 correction? */
					   cm->hbmx,          /* the HMM banded matrix */
					   32768.,            /* upper limit for size of DP matrix, 32 Gb */
					   &inssc)) != eslOK) { /* best score, irrelevant here */
	      printf("ERROR: %s\n", errbuf); return status;  }
	    printf("\t\t\tFULL HB INS %7.2f bits\n", inssc);
	  }
	  else { /* final algorithm is HMM banded CYK */
	    /*printf("calling HMM banded CYK scan\n");*/
	    if((status = FastCYKScanHB(cm, errbuf, subseq, dstarts[d], dends[d], 
				       pli->T,            /* minimum score to report */
				       results,           /* our results data structure that will store hit(s) */
				       pli->do_null3,     /* do the NULL3 correction? */
				       cm->hbmx,          /* the HMM banded matrix */
				       32768.,            /* upper limit for size of DP matrix, 32 Gb */
				       &cyksc)) != eslOK) { /* best score, irrelevant here */
	      printf("ERROR: %s\n", errbuf); return status;  }
	    printf("\t\t\tFULL HB CYK %7.2f bits\n", cyksc);
	  }
	}
	else { 
	  /*******************************************************************
	   * Run non-HMM banded (probably qdb) version of CYK or Inside *
	   *******************************************************************/
	  if(cm->search_opts & CM_SEARCH_INSIDE) { /* final algorithm is Inside */
	    /*printf("calling non-HMM banded Inside scan\n");*/
	    if((status = FastIInsideScan(cm, errbuf, pli->smx, subseq, dstarts[d], dends[d],
					 pli->T,            /* minimum score to report */
					 results,           /* our results data structure that will store hit(s) */
					 pli->do_null3,     /* apply the null3 correction? */
					 NULL,              /* ret_vsc, irrelevant here */
					 NULL)) != eslOK) { /* best score, irrelevant here */
	      printf("ERROR: %s\n", errbuf); return status; }
	  }
	  else { /* final algorithm is CYK */
	    /*printf("calling non-HMM banded CYK scan\n");*/
	    if((status = FastCYKScan(cm, errbuf, pli->fsmx, subseq, dstarts[d], dends[d],
				     pli->T,            /* minimum score to report */
				     results,           /* our results data structure that will store hit(s) */
				     pli->do_null3,     /* apply the null3 correction? */
				     NULL,              /* ret_vsc, irrelevant here */
				     NULL)) != eslOK) { /* best score, irrelevant here */
	      printf("ERROR: %s\n", errbuf); return status; }
	  }
	}
	/*printf("FINAL: %.2f\n", finalsc);*/
	/* add each hit to the hitlist */
	for (h = 0; h < results->num_results; h++) { 
	  p7_tophits_CreateNextHit(hitlist, &hit);
	  hit->ndom        = 1;
	  hit->best_domain = 0;
	  /*hit->window_length = ESL_MIN(om->max_length, window_len); //see notes, ~/notebook/20100716_hmmer_score_v_eval_bug/, end of Thu Jul 22 13:36:49 EDT 2010*/
	  hit->subseq_start = sq->start;
	  
	  ESL_ALLOC(hit->dcl, sizeof(P7_DOMAIN));
	  
	  hit->dcl[0].ienv = hit->dcl[0].iali = results->data[h].start + (window_starts[i] - 1);
	  hit->dcl[0].jenv = hit->dcl[0].jali = results->data[h].stop  + (window_starts[i] - 1);
	  hit->dcl[0].bitscore = hit->dcl[0].envsc = results->data[h].score;
	  hit->dcl[0].pvalue   = esl_exp_surv(results->data[h].score, cm->stats->expAA[pli->final_cm_exp_mode][0]->mu_extrap, cm->stats->expAA[pli->final_cm_exp_mode][0]->lambda);
	  /* initialize remaining values we don't know yet */
	  hit->dcl[0].domcorrection = 0.;
	  hit->dcl[0].dombias  = 0.;
	  hit->dcl[0].oasc     = 0.;
	  hit->dcl[0].is_reported = hit->dcl[0].is_included = FALSE;
	  hit->dcl[0].ad = NULL;
	  
	  hit->pre_score  = hit->sum_score  = hit->score  = hit->dcl[0].bitscore;
	  hit->pre_pvalue = hit->sum_pvalue = hit->pvalue = hit->dcl[0].pvalue;
	  
	  if (pli->mode == CM_SEARCH_SEQS) { 
	    if (                       (status  = esl_strdup(sq->name, -1, &(hit->name)))  != eslOK) ESL_EXCEPTION(eslEMEM, "allocation failure");
	    if (sq->acc[0]  != '\0' && (status  = esl_strdup(sq->acc,  -1, &(hit->acc)))   != eslOK) ESL_EXCEPTION(eslEMEM, "allocation failure");
	  } 
	  else {
	    if ((status  = esl_strdup(om->name, -1, &(hit->name)))  != eslOK) esl_fatal("allocation failure");
	    if ((status  = esl_strdup(om->acc,  -1, &(hit->acc)))   != eslOK) esl_fatal("allocation failure");
	  }
	  printf("\t\t\tIns h: %2d  [%7d..%7d]  %7.2f bits  E: %g\n", h+1, hit->dcl[0].ienv, hit->dcl[0].jenv, hit->dcl[0].bitscore, hit->dcl[0].pvalue);
	}
	FreeResults(results);
      } 
      pli->ddef->ndom = 0; // reset for next use
    } /* end of else (entered if pli->do_cm */
  }
    
  return eslOK;

  ERROR:
  ESL_EXCEPTION(eslEMEM, "Error allocating memory for hit list in pipeline\n");

}


/* Function:  cm_pli_Statistics()
 * Synopsis:  Final statistics output from a processing pipeline.
 * Incept:    EPN, Fri Sep 24 16:48:06 2010 
 *            SRE, Tue Dec  9 10:19:45 2008 [Janelia] (p7_pli_Statistics())
 *
 * Purpose:   Print a standardized report of the internal statistics of
 *            a finished processing pipeline <pli> to stream <ofp>.
 *            
 *            If stopped, non-<NULL> stopwatch <w> is provided for a
 *            stopwatch that was timing the pipeline, then the report
 *            includes timing information.
 *
 * Returns:   <eslOK> on success.
 */
int
cm_pli_Statistics(FILE *ofp, CM_PIPELINE *pli, ESL_STOPWATCH *w)
{
  double ntargets; 

  fprintf(ofp, "Internal pipeline statistics summary:\n");
  fprintf(ofp, "-------------------------------------\n");
  if (pli->mode == CM_SEARCH_SEQS) {
    fprintf(ofp,   "Query model(s):               %15" PRId64 "  (%" PRId64 " consensus positions)\n",     pli->nmodels, pli->nnodes);
    fprintf(ofp,   "Target sequences:             %15" PRId64 "  (%" PRId64 " residues)\n",  pli->nseqs,   pli->nres);
    ntargets = pli->nseqs;
  } else {
    fprintf(ofp, "Query sequence(s):                    %15" PRId64 "  (%" PRId64 " residues)\n",  pli->nseqs,   pli->nres);
    fprintf(ofp, "Target model(s):                      %15" PRId64 "  (%" PRId64 " nodes)\n",     pli->nmodels, pli->nnodes);
    ntargets = pli->nmodels;
  }

  if(pli->do_msv) { 
    fprintf(ofp, "Windows passing MSV filter:   %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_msv,
	    (double)pli->pos_past_msv / pli->nres ,
	    pli->F1);
  }
  else { 
    fprintf(ofp, "Windows passing MSV filter:   %15s  (off)\n", "");
  }

  if(pli->do_biasfilter) { 
    fprintf(ofp, "Windows passing bias filter:  %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_bias,
	    (double)pli->pos_past_bias / pli->nres ,
	    pli->F1);
  }
  else { 
    fprintf(ofp, "Windows passing bias filter:  %15s  (off)\n", "");
  }

  if(pli->do_vit) { 
    fprintf(ofp, "Windows passing Vit filter:   %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_vit,
	    (double)pli->pos_past_vit / pli->nres ,
	    pli->F2);
  }
  else { 
    fprintf(ofp, "Windows passing Vit filter:   %15s  (off)\n", "");
  }
  
  if(pli->do_fwd) { 
    fprintf(ofp, "Windows passing Fwd filter:   %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_fwd,
	    (double)pli->pos_past_fwd / pli->nres ,
	    pli->F3);
  }
  else { 
    fprintf(ofp, "Windows passing Fwd filter:   %15s  (off)\n", "");
  }

  if(pli->do_cyk) { 
    fprintf(ofp, "Windows passing CYK filter:   %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_cyk,
	    (double)pli->pos_past_cyk / pli->nres ,
	    pli->F4);
  }
  else { 
    fprintf(ofp, "Windows passing CYK filter:   %15s  (off)\n", "");
  }

  fprintf(ofp, "Total hits:                   %15d  (%.4g)\n",
          (int)pli->n_output,
          (double)pli->pos_output / pli->nres );

  if (w != NULL) {
    esl_stopwatch_Display(ofp, w, "# CPU time: ");
    fprintf(ofp, "# Mc/sec: %.2f\n", 
        (double) pli->nres * (double) pli->nnodes / (w->elapsed * 1.0e6));
  }

  return eslOK;
}
/*------------------- end, pipeline API -------------------------*/


/*****************************************************************
 * 3. Example 1: "search mode" in a sequence db
 *****************************************************************/

#ifdef p7PIPELINE_EXAMPLE
/* gcc -o pipeline_example -g -Wall -I../easel -L../easel -I. -L. -Dp7PIPELINE_EXAMPLE p7_pipeline.c -lhmmer -leasel -lm
 * ./pipeline_example <hmmfile> <sqfile>
 */

#include "p7_config.h"

#include "easel.h"
#include "esl_getopts.h"
#include "esl_sqio.h"

#include "hmmer.h"

static ESL_OPTIONS options[] = {
  /* name           type         default   env  range   toggles   reqs   incomp                             help                                                  docgroup*/
  { "-h",           eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  NULL,                          "show brief help on version and usage",                         0 },
  { "-E",           eslARG_REAL,  "10.0", NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "E-value cutoff for reporting significant sequence hits",       0 },
  { "-T",           eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "bit score cutoff for reporting significant sequence hits",     0 },
  { "-Z",           eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  NULL,                          "set # of comparisons done, for E-value calculation",           0 },
  { "--domE",       eslARG_REAL,"1000.0", NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "E-value cutoff for reporting individual domains",              0 },
  { "--domT",       eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "bit score cutoff for reporting individual domains",            0 },
  { "--domZ",       eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  NULL,                          "set # of significant seqs, for domain E-value calculation",    0 },
  { "--cut_ga",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use GA gathering threshold bit score cutoffs in <hmmfile>",    0 },
  { "--cut_nc",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use NC noise threshold bit score cutoffs in <hmmfile>",        0 },
  { "--cut_tc",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use TC trusted threshold bit score cutoffs in <hmmfile>",      0 },
  { "--max",        eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, "--F1,--F2,--F3",               "Turn all heuristic filters off (less speed, more power)",      0 },
  { "--F1",         eslARG_REAL,  "0.02", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 1 (MSV) threshold: promote hits w/ P <= F1",             0 },
  { "--F2",         eslARG_REAL,  "1e-3", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 2 (Vit) threshold: promote hits w/ P <= F2",             0 },
  { "--F3",         eslARG_REAL,  "1e-5", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 3 (Fwd) threshold: promote hits w/ P <= F3",             0 },
  { "--nobias",     eslARG_NONE,   NULL,  NULL, NULL,      NULL,  NULL, "--max",                        "turn off composition bias filter",                             0 },
  { "--nonull2",    eslARG_NONE,   NULL,  NULL, NULL,      NULL,  NULL,  NULL,                          "turn off biased composition score corrections",                0 },
  { "--seed",       eslARG_INT,    "42",  NULL, "n>=0",    NULL,  NULL,  NULL,                          "set RNG seed to <n> (if 0: one-time arbitrary seed)",          0 },
  { "--acc",        eslARG_NONE,  FALSE,  NULL, NULL,      NULL,  NULL,  NULL,                          "output target accessions instead of names if possible",        0 },
 {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <hmmfile> <seqdb>";
static char banner[] = "example of using acceleration pipeline in search mode (seq targets)";

int
main(int argc, char **argv)
{
  ESL_GETOPTS  *go      = esl_getopts_CreateDefaultApp(options, 2, argc, argv, banner, usage);
  char         *hmmfile = esl_opt_GetArg(go, 1);
  char         *seqfile = esl_opt_GetArg(go, 2);
  int           format  = eslSQFILE_FASTA;
  P7_HMMFILE   *hfp     = NULL;
  ESL_ALPHABET *abc     = NULL;
  P7_BG        *bg      = NULL;
  P7_HMM       *hmm     = NULL;
  P7_PROFILE   *gm      = NULL;
  P7_OPROFILE  *om      = NULL;
  ESL_SQFILE   *sqfp    = NULL;
  ESL_SQ       *sq      = NULL;
  CM_PIPELINE  *pli     = NULL;
  P7_TOPHITS   *hitlist = NULL;
  int           h,d,namew;

  /* Don't forget this. Null2 corrections need FLogsum() */
  p7_FLogsumInit();

  /* Read in one HMM */
  if (p7_hmmfile_Open(hmmfile, NULL, &hfp) != eslOK) p7_Fail("Failed to open HMM file %s", hmmfile);
  if (p7_hmmfile_Read(hfp, &abc, &hmm)     != eslOK) p7_Fail("Failed to read HMM");
  p7_hmmfile_Close(hfp);

  /* Open a sequence file */
  if (esl_sqfile_OpenDigital(abc, seqfile, format, NULL, &sqfp) != eslOK) p7_Fail("Failed to open sequence file %s\n", seqfile);
  sq = esl_sq_CreateDigital(abc);

  /* Create a pipeline and a top hits list */
  pli     = p7_pipeline_Create(go, hmm->M, 400, FALSE, p7_SEARCH_SEQS);
  hitlist = p7_tophits_Create();

  /* Configure a profile from the HMM */
  bg = p7_bg_Create(abc);
  gm = p7_profile_Create(hmm->M, abc);
  om = p7_oprofile_Create(hmm->M, abc);
  p7_ProfileConfig(hmm, bg, gm, 400, p7_LOCAL);
  p7_oprofile_Convert(gm, om);     /* <om> is now p7_LOCAL, multihit */
  p7_pli_NewModel(pli, om, bg);

  /* Run each target sequence through the pipeline */
  while (esl_sqio_Read(sqfp, sq) == eslOK)
    { 
      p7_pli_NewSeq(pli, sq);
      p7_bg_SetLength(bg, sq->n);
      p7_oprofile_ReconfigLength(om, sq->n);
  
      p7_Pipeline(pli, om, bg, sq, hitlist);

      esl_sq_Reuse(sq);
      p7_pipeline_Reuse(pli);
    }

  /* Print the results. 
   * This example is a stripped version of hmmsearch's tabular output.
   */
  p7_tophits_Sort(hitlist);
  namew = ESL_MAX(8, p7_tophits_GetMaxNameLength(hitlist));
  for (h = 0; h < hitlist->N; h++)
    {
      d    = hitlist->hit[h]->best_domain;

      printf("%10.2g %7.1f %6.1f  %7.1f %6.1f %10.2g  %6.1f %5d  %-*s %s\n",
         hitlist->hit[h]->pvalue * (double) pli->Z,
         hitlist->hit[h]->score,
         hitlist->hit[h]->pre_score - hitlist->hit[h]->score, /* bias correction */
         hitlist->hit[h]->dcl[d].bitscore,
         eslCONST_LOG2R * p7_FLogsum(0.0, log(bg->omega) + hitlist->hit[h]->dcl[d].domcorrection), /* print in units of bits */
         hitlist->hit[h]->dcl[d].pvalue * (double) pli->Z,
         hitlist->hit[h]->nexpected,
         hitlist->hit[h]->nreported,
         namew,
         hitlist->hit[h]->name,
         hitlist->hit[h]->desc);
    }

  /* Done. */
  p7_tophits_Destroy(hitlist);
  p7_pipeline_Destroy(pli);
  esl_sq_Destroy(sq);
  esl_sqfile_Close(sqfp);
  p7_oprofile_Destroy(om);
  p7_profile_Destroy(gm);
  p7_hmm_Destroy(hmm);
  p7_bg_Destroy(bg);
  esl_alphabet_Destroy(abc);
  esl_getopts_Destroy(go);
  return 0;
}
#endif /*p7PIPELINE_EXAMPLE*/
/*----------- end, search mode (seq db) example -----------------*/




/*****************************************************************
 * 4. Example 2: "scan mode" in an HMM db
 *****************************************************************/
#ifdef p7PIPELINE_EXAMPLE2
/* gcc -o pipeline_example2 -g -Wall -I../easel -L../easel -I. -L. -Dp7PIPELINE_EXAMPLE2 p7_pipeline.c -lhmmer -leasel -lm
 * ./pipeline_example2 <hmmdb> <sqfile>
 */

#include "p7_config.h"

#include "easel.h"
#include "esl_getopts.h"
#include "esl_sqio.h"

#include "hmmer.h"

static ESL_OPTIONS options[] = {
  /* name           type         default   env  range   toggles   reqs   incomp                             help                                                  docgroup*/
  { "-h",           eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  NULL,                          "show brief help on version and usage",                         0 },
  { "-E",           eslARG_REAL,  "10.0", NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "E-value cutoff for reporting significant sequence hits",       0 },
  { "-T",           eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "bit score cutoff for reporting significant sequence hits",     0 },
  { "-Z",           eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  NULL,                          "set # of comparisons done, for E-value calculation",           0 },
  { "--domE",       eslARG_REAL,"1000.0", NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "E-value cutoff for reporting individual domains",              0 },
  { "--domT",       eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "bit score cutoff for reporting individual domains",            0 },
  { "--domZ",       eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  NULL,                          "set # of significant seqs, for domain E-value calculation",    0 },
  { "--cut_ga",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use GA gathering threshold bit score cutoffs in <hmmfile>",    0 },
  { "--cut_nc",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use NC noise threshold bit score cutoffs in <hmmfile>",        0 },
  { "--cut_tc",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use TC trusted threshold bit score cutoffs in <hmmfile>",      0 },
  { "--max",        eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, "--F1,--F2,--F3",               "Turn all heuristic filters off (less speed, more power)",      0 },
  { "--F1",         eslARG_REAL,  "0.02", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 1 (MSV) threshold: promote hits w/ P <= F1",             0 },
  { "--F2",         eslARG_REAL,  "1e-3", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 2 (Vit) threshold: promote hits w/ P <= F2",             0 },
  { "--F3",         eslARG_REAL,  "1e-5", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 3 (Fwd) threshold: promote hits w/ P <= F3",             0 },
  { "--nobias",     eslARG_NONE,   NULL,  NULL, NULL,      NULL,  NULL, "--max",                        "turn off composition bias filter",                             0 },
  { "--nonull2",    eslARG_NONE,   NULL,  NULL, NULL,      NULL,  NULL,  NULL,                          "turn off biased composition score corrections",                0 },
  { "--seed",       eslARG_INT,    "42",  NULL, "n>=0",    NULL,  NULL,  NULL,                          "set RNG seed to <n> (if 0: one-time arbitrary seed)",          0 },
  { "--acc",        eslARG_NONE,  FALSE,  NULL, NULL,      NULL,  NULL,  NULL,                          "output target accessions instead of names if possible",        0 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <hmmfile> <seqfile>";
static char banner[] = "example of using acceleration pipeline in scan mode (HMM targets)";

int
main(int argc, char **argv)
{
  ESL_GETOPTS  *go      = esl_getopts_CreateDefaultApp(options, 2, argc, argv, banner, usage);
  char         *hmmfile = esl_opt_GetArg(go, 1);
  char         *seqfile = esl_opt_GetArg(go, 2);
  int           format  = eslSQFILE_FASTA;
  P7_HMMFILE   *hfp     = NULL;
  ESL_ALPHABET *abc     = NULL;
  P7_BG        *bg      = NULL;
  P7_OPROFILE  *om      = NULL;
  ESL_SQFILE   *sqfp    = NULL;
  ESL_SQ       *sq      = NULL;
  CM_PIPELINE  *pli     = NULL;
  P7_TOPHITS   *hitlist = p7_tophits_Create();
  int           h,d,namew;

  /* Don't forget this. Null2 corrections need FLogsum() */
  p7_FLogsumInit();

  /* Open a sequence file, read one seq from it.
   * Convert to digital later, after 1st HMM is input and abc becomes known 
   */
  sq = esl_sq_Create();
  if (esl_sqfile_Open(seqfile, format, NULL, &sqfp) != eslOK) p7_Fail("Failed to open sequence file %s\n", seqfile);
  if (esl_sqio_Read(sqfp, sq)                       != eslOK) p7_Fail("Failed to read sequence from %s\n", seqfile);
  esl_sqfile_Close(sqfp);

  /* Open the HMM db */
  if (p7_hmmfile_Open(hmmfile, NULL, &hfp) != eslOK) p7_Fail("Failed to open HMM file %s", hmmfile);

  /* Create a pipeline for the query sequence in scan mode */
  pli      = p7_pipeline_Create(go, 100, sq->n, FALSE, p7_SCAN_MODELS);
  p7_pli_NewSeq(pli, sq);
   
  /* Some additional config of the pipeline specific to scan mode */
  pli->hfp = hfp;
  if (! pli->Z_is_fixed && hfp->is_pressed) { pli->Z_is_fixed = TRUE; pli->Z = hfp->ssi->nprimary; }

  /* Read (partial) of each HMM in file */
  while (p7_oprofile_ReadMSV(hfp, &abc, &om) == eslOK) 
    {
      /* One time only initialization after abc becomes known */
      if (bg == NULL) 
    {
      bg = p7_bg_Create(abc);
      if (esl_sq_Digitize(abc, sq) != eslOK) p7_Die("alphabet mismatch");
      p7_bg_SetLength(bg, sq->n);
    }
      p7_pli_NewModel(pli, om, bg);
      p7_oprofile_ReconfigLength(om, sq->n);

      p7_Pipeline(pli, om, bg, sq, hitlist);
      
      p7_oprofile_Destroy(om);
      p7_pipeline_Reuse(pli);
    } 

  /* Print the results. 
   * This example is a stripped version of hmmsearch's tabular output.
   */
  p7_tophits_Sort(hitlist);
  namew = ESL_MAX(8, p7_tophits_GetMaxNameLength(hitlist));
  for (h = 0; h < hitlist->N; h++)
    {
      d    = hitlist->hit[h]->best_domain;

      printf("%10.2g %7.1f %6.1f  %7.1f %6.1f %10.2g  %6.1f %5d  %-*s %s\n",
         hitlist->hit[h]->pvalue * (double) pli->Z,
         hitlist->hit[h]->score,
         hitlist->hit[h]->pre_score - hitlist->hit[h]->score, /* bias correction */
         hitlist->hit[h]->dcl[d].bitscore,
         eslCONST_LOG2R * p7_FLogsum(0.0, log(bg->omega) + hitlist->hit[h]->dcl[d].domcorrection), /* print in units of BITS */
         hitlist->hit[h]->dcl[d].pvalue * (double) pli->Z,
         hitlist->hit[h]->nexpected,
         hitlist->hit[h]->nreported,
         namew,
         hitlist->hit[h]->name,
         hitlist->hit[h]->desc);
    }

  /* Done. */
  p7_tophits_Destroy(hitlist);
  p7_pipeline_Destroy(pli);
  esl_sq_Destroy(sq);
  p7_hmmfile_Close(hfp);
  p7_bg_Destroy(bg);
  esl_alphabet_Destroy(abc);
  esl_getopts_Destroy(go);
  return 0;
}
#endif /*p7PIPELINE_EXAMPLE2*/
/*--------------- end, scan mode (HMM db) example ---------------*/



/*****************************************************************
 * @LICENSE@
 *****************************************************************/
