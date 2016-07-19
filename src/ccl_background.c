#include "ccl_background.h"
#include "ccl_utils.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "gsl/gsl_errno.h"
//#include "gsl/gsl_odeiv.h"
#include "gsl/gsl_odeiv2.h"
#include "gsl/gsl_spline.h"
#include "gsl/gsl_integration.h"

//TODO: is it worth separating between cases for speed purposes?
//E.g. flat vs non-flat, LDCM vs wCDM
//CHANGED: modified this to include non-flat cosmologies
static double h_over_h0(double a, ccl_parameters * params)
{
  return sqrt((params->Omega_m+params->Omega_l*pow(a,-3*(params->w0+params->wa))*
	       exp(3*params->wa*(a-1))+params->Omega_k*a)/(a*a*a));
}

static double omega_m(double a,ccl_parameters * params)
{
  return params->Omega_m/(params->Omega_m+params->Omega_l*pow(a,-3*(params->w0+params->wa))*
			  exp(3*params->wa*(a-1))+params->Omega_k*a);
}

static double chi_integrand(double a, void * cosmo_void)
{
  ccl_cosmology * cosmo = cosmo_void;
  //TODO: length units here are Mpc/h
  return CLIGHT_HMPC/(a*a*h_over_h0(a, &(cosmo->params)));
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// Growth function for w0-wa models  - PLACEHOLDER CODE!
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

//CHANGED: generalized to non-flat cosmologies

/*
//function for growfac (DGL)
int growth_integrand(double a,const double y[],double f[],void *params)
{
    double *p=(double *)params;
    if (a == 0) {
        fprintf(stderr, "a=0 in function 'func_for_growfac'!\n");
        return 1;
    }
    double aa=a*a;
    double omegam=p[0]/(aa*a);
    double omegav=p[1]*exp(-3.*((p[2]+p[3]+1)*log(a)+p[3]*(1.-a)));
    double hub=omegam+(1-p[0]-p[1])/(a*a)+omegav;
    f[0]=y[1];
    f[1]=y[0]*3.*p[0]/(2.*hub*aa*aa*a)-y[1]/a*(2.-(omegam+(3.*(p[2]+p[3]*(1.-a))+1)*omegav)/(2.*hub));
    return GSL_SUCCESS;
}
//TODO: check this

static
int growth_function_array(double *a, double *table, int na, ccl_parameters * params)
{
  
    const gsl_odeiv_step_type *T=gsl_odeiv_step_rkf45;
    gsl_odeiv_step *s=gsl_odeiv_step_alloc(T,2);
    gsl_odeiv_control *c=gsl_odeiv_control_y_new(1.e-6,0.0);
    gsl_odeiv_evolve *e=gsl_odeiv_evolve_alloc(2);

    double t=1e-6;            //start a
    double h=1.e-6;           //initial step size
    double y[2]={t,t};        //initial conditions
    double par[4]={params->Omega_m,params->Omega_l,params->w0,params->wa};
    gsl_odeiv_system sys={growth_integrand,NULL,2,&par};
    int status = 0;
    for (int i=0; i<na; i++) {
        while(t<a[i]){
            status |= gsl_odeiv_evolve_apply(e,c,s,&sys,&t,a[i],&h,y);
            if (status) break;
        }
        table[i]=y[0];
    }
    for (int i=0; i<na; i++) {
        table[i] /= table[na-1];
    }    
    gsl_odeiv_evolve_free(e);
    gsl_odeiv_control_free(c);
    gsl_odeiv_step_free(s);

  return status;
}
//TODO: check this
*/

static int growth_ode_system(double a,const double y[],double dydt[],void *params)
{
  ccl_cosmology * cosmo = params;
  double hnorm=h_over_h0(a,&(cosmo->params));
  double om=omega_m(a,&(cosmo->params));

  dydt[0]=y[1]/(a*a*a*hnorm);
  dydt[1]=1.5*hnorm*a*om*y[0];

  return GSL_SUCCESS;
}

static int growth_factor_and_growth_rate(double a,double *gf,double *fg,ccl_cosmology *cosmo)
{
  if(a<EPS_SCALEFAC_GROWTH) {
    *gf=a;
    *fg=1;
    return 0;
  }
  else {
    double y[2];
    double ainit=EPS_SCALEFAC_GROWTH;
    gsl_odeiv2_system sys={growth_ode_system,NULL,2,cosmo};
    gsl_odeiv2_driver *d=
      gsl_odeiv2_driver_alloc_y_new(&sys,gsl_odeiv2_step_rkck,0.1*EPS_SCALEFAC_GROWTH,0,EPSREL_GROWTH);

    y[0]=EPS_SCALEFAC_GROWTH;
    y[1]=EPS_SCALEFAC_GROWTH*EPS_SCALEFAC_GROWTH*EPS_SCALEFAC_GROWTH*
      h_over_h0(EPS_SCALEFAC_GROWTH,&(cosmo->params));


    int status=gsl_odeiv2_driver_apply(d,&ainit,a,y);
    gsl_odeiv2_driver_free(d);
    
    if(status!=GSL_SUCCESS) {
      fprintf(stderr,"ODE didn't converge when computing growth\n");
      return 1;
    }
    
    *gf=y[0];
    *fg=y[1]/(a*a*h_over_h0(a,&(cosmo->params))*y[0]);

    return 0;
  }
}

void ccl_cosmology_compute_distances(ccl_cosmology * cosmo, int *status)
{
  if(cosmo->computed_distances)
    return;

  // Create linearly-spaced values of the scale factor
  int na=0;
  double * a = ccl_linear_spacing(A_SPLINE_MIN, A_SPLINE_MAX, A_SPLINE_DELTA, &na);
  if (a==NULL || 
      (fabs(a[0]-A_SPLINE_MIN)>1e-5) || 
      (fabs(a[na-1]-A_SPLINE_MAX)>1e-5) || 
      (a[na-1]>1.0)
      ) {
    fprintf(stderr, "Error creating linear spacing.\n");        
    *status = 1;
    return;
  }

  // allocate space for y, which will be all three
  // of E(a), chi(a), D(a) and f(a) in turn.
  double *y = malloc(sizeof(double)*na);

  // Fill in E(a)
  for (int i=0; i<na; i++){
    y[i] = h_over_h0(a[i], &cosmo->params);
  }

  // Allocate and fill E spline with values we just got
  gsl_spline * E = gsl_spline_alloc(A_SPLINE_TYPE, na);
  *status = gsl_spline_init(E, a, y, na);
  // Check for errors in creating the spline
  if (*status){
    free(a);
    free(y);
    gsl_spline_free(E);
    fprintf(stderr, "Error creating E(a) spline\n");
    return;
  }

  //Fill in chi(a)
  //TODO: CQUAD is great, but slower than other methods. This could be sped up if it becomes an issue.
  //TODO: check length units
  gsl_integration_cquad_workspace * workspace = gsl_integration_cquad_workspace_alloc (1000);
  gsl_function F;
  F.function = &chi_integrand;
  F.params = cosmo;
  for (int i=0; i<na; i++){
    *status |= gsl_integration_cquad(&F, a[i], 1.0, 0.0,EPSREL_DIST,workspace,&y[i], NULL, NULL); 
  }
  gsl_integration_cquad_workspace_free(workspace);
  if (*status){
    free(a);
    free(y);
    gsl_spline_free(E);        
    fprintf(stderr, "Error integrating to get chi(a)\n");
    return;
  }

  gsl_spline * chi = gsl_spline_alloc(A_SPLINE_TYPE, na);
  *status = gsl_spline_init(chi, a, y, na);
  if (*status){
    free(a);
    free(y);
    gsl_spline_free(E);
    gsl_spline_free(chi);
    fprintf(stderr, "Error creating chi(a) spline\n");
    return;
  }

  //Fill in D(a) and f(a)
  double *y2 = malloc(sizeof(double)*na);
  for (int i=0; i<na; i++){
    *status |= growth_factor_and_growth_rate(a[i],&(y[i]),&(y2[i]),cosmo);
  }
  if (*status){
    free(a);
    free(y);
    free(y2);
    gsl_spline_free(E);
    gsl_spline_free(chi);
    fprintf(stderr, "Error creating growth array\n");
    return;
  }

  gsl_spline * growth = gsl_spline_alloc(A_SPLINE_TYPE, na);
  *status = gsl_spline_init(growth, a, y, na);
  if (*status){
    free(a);
    free(y);
    free(y2);
    gsl_spline_free(E);
    gsl_spline_free(chi);
    gsl_spline_free(growth);
    fprintf(stderr, "Error creating growth spline\n");
    return;
  }
  gsl_spline * fgrowth = gsl_spline_alloc(A_SPLINE_TYPE, na);
  *status = gsl_spline_init(fgrowth, a, y2, na);
  if (*status){
    free(a);
    free(y);
    free(y2);
    gsl_spline_free(E);
    gsl_spline_free(chi);
    gsl_spline_free(growth);
    gsl_spline_free(fgrowth);
    fprintf(stderr, "Error creating growth spline\n");
    return;
  }

  // Initialize the accelerator which speeds the splines and 
  // assign all the splines we've just made to the structure.
  cosmo->data.accelerator=gsl_interp_accel_alloc();
  cosmo->data.E = E;
  cosmo->data.chi = chi;
  cosmo->data.growth = growth;
  cosmo->data.fgrowth = fgrowth;
  cosmo->computed_distances = true;
  
  free(a);
  free(y);
  free(y2);

  return;
}

// Distance-like function examples

double ccl_comoving_radial_distance(ccl_cosmology * cosmo, double a)
{
   return gsl_spline_eval(cosmo->data.chi, a, cosmo->data.accelerator);
}

int ccl_comoving_radial_distances(ccl_cosmology * cosmo, int na, double a[na], double output[na])
{
  for (int i=0; i<na; i++){
    output[i]=gsl_spline_eval(cosmo->data.chi,a[i],cosmo->data.accelerator);
  }

  return 0;
}
//TODO: do this

double ccl_luminosity_distance(ccl_cosmology * cosmo, double a)
{
    return ccl_comoving_radial_distance(cosmo, a) / a;
}
//TODO: this is not valid for curved cosmologies

int ccl_luminosity_distances(ccl_cosmology * cosmo, int na, double a[na], double output[na])
{
  for (int i=0; i<na; i++){
    output[i]=gsl_spline_eval(cosmo->data.chi,a[i],cosmo->data.accelerator)/a[i];
  }

  return 0;
}
//TODO: do this

double ccl_growth_factor(ccl_cosmology * cosmo, double a, int * status)
{
    return gsl_spline_eval(cosmo->data.growth, a, cosmo->data.accelerator);
}


int ccl_growth_factors(ccl_cosmology * cosmo, int na, double a[na], double output[na])
{
  for (int i=0; i<na; i++){
    output[i]=gsl_spline_eval(cosmo->data.growth,a[i],cosmo->data.accelerator);
  }

  return 0;
}
//TODO: do this

double ccl_growth_rate(ccl_cosmology * cosmo, double a, int * status)
{
    return gsl_spline_eval(cosmo->data.fgrowth, a, cosmo->data.accelerator);
}

int ccl_growth_rates(ccl_cosmology * cosmo, int na, double a[na], double output[na])
{
  for (int i=0; i<na; i++){
    output[i]=gsl_spline_eval(cosmo->data.fgrowth,a[i],cosmo->data.accelerator);
  }

  return 0;
}
//TODO: do this


// Power function examples
