/*$Id: sbaijspooles.c,v 1.10 2001/08/15 15:56:50 bsmith Exp $*/
/* 
   Provides an interface to the Spooles serial sparse solver
*/

#include "src/mat/impls/sbaij/seq/sbaij.h"

#if defined(PETSC_HAVE_SPOOLES) && !defined(PETSC_USE_SINGLE) && !defined(PETSC_USE_COMPLEX)
#include "src/mat/impls/aij/seq/spooles.h"
/* Note the Petsc r permutation is ignored */
#undef __FUNCT__  
#define __FUNCT__ "MatCholeskyFactorSymbolic_SeqSBAIJ_Spooles"
int MatCholeskyFactorSymbolic_SeqSBAIJ_Spooles(Mat A,IS r,PetscReal f,Mat *F)
{ 
  Mat_Spooles          *lu;   
  int                  ierr,m=A->m,n=A->n;

  PetscFunctionBegin;	
  /* Create the factorization matrix F */  
  ierr = MatCreateSeqAIJ(A->comm,m,n,PETSC_NULL,PETSC_NULL,F);CHKERRQ(ierr);

  (*F)->ops->choleskyfactornumeric  = MatLUFactorNumeric_SeqAIJ_Spooles;
  (*F)->factor                      = FACTOR_CHOLESKY;  

  ierr = PetscNew(Mat_Spooles,&lu);CHKERRQ(ierr); 
  (*F)->spptr       = (void*)lu;
  lu->symflag       = SPOOLES_SYMMETRIC;
  lu->pivotingflag  = SPOOLES_NO_PIVOTING;
  lu->flg           = DIFFERENT_NONZERO_PATTERN;

  PetscFunctionReturn(0); 
}

#undef __FUNCT__  
#define __FUNCT__ "MatUseSpooles_SeqSBAIJ"
int MatUseSpooles_SeqSBAIJ(Mat A)
{
  Mat_SeqSBAIJ *sbaij = (Mat_SeqSBAIJ*)A->data;
  int          bs = sbaij->bs;

  PetscFunctionBegin;
  if (bs > 1) SETERRQ1(1,"Block size %d not supported by Spooles",bs);
  A->ops->choleskyfactorsymbolic = MatCholeskyFactorSymbolic_SeqSBAIJ_Spooles;  
  PetscFunctionReturn(0);
}

#else

#undef __FUNCT__  
#define __FUNCT__ "MatUseSpooles_SeqSBAIJ"
int MatUseSpooles_SeqSBAIJ(Mat A)
{
  PetscFunctionBegin;
  PetscFunctionReturn(0);
}

#endif


