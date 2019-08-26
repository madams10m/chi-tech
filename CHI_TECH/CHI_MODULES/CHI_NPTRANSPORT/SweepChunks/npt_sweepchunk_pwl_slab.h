#ifndef _npt_sweepchunk_pwl_slab_h
#define _npt_sweepchunk_pwl_slab_h


#include <CHI_MESH/CHI_MESHCONTINUUM/chi_meshcontinuum.h>
#include <CHI_MESH/CHI_SWEEP/chi_sweep.h>
#include <chi_discretization.h>
#include <CHI_DISCRETIZATION_PWL/pwl.h>
#include <CHI_DISCRETIZATION_PWL/CellViews/pwl_slab.h>
#include <CHI_DISCRETIZATION_PWL/CellViews/pwl_polyhedron.h>

#include <CHI_MESH/CHI_CELL/cell_polyhedron.h>
#include <CHI_PHYSICS/chi_physics.h>

#include <CHI_MATH/chi_math.h>
#include "../GroupSet/npt_groupset.h"
#include <CHI_MATH/Quadratures/product_quadrature.h>

#include <CHI_MESH/CHI_SWEEP/chi_SPDS.h>
#include <CHI_MESH/CHI_SWEEP/chi_angleaggregation.h>

#include <CHI_TIMER/chi_timer.h>

#include <chi_mpi.h>
#include <chi_log.h>

extern CHI_MATH    chi_math_handler;
extern CHI_MPI     chi_mpi;
extern CHI_LOG     chi_log;

typedef std::vector<chi_physics::TransportCrossSections*> TCrossSections;

//###################################################################
/**Sweep chunk to compute the fixed source.*/
class NPT_SWEEP_PWL_SLAB : public chi_mesh::SweepManagement::SweepChunk
{
private:
  chi_mesh::MeshContinuum*    grid_view;
  CHI_DISCRETIZATION_PWL*     grid_fe_view;
  std::vector<NPT_CELLVIEW*>* grid_transport_view;
//std::vector<double>*        x;                   BASE CLASS
  std::vector<double>*        q_moments;
  NPT_GROUPSET*               groupset;
  TCrossSections*             xsections;
  int                         num_moms;

  int                         G;
  int                         g;
  chi_mesh::Vector            omega;
  double                      wn;

  int                         max_cell_dofs;

  bool                        a_and_b_initialized;

//bool                        suppress_surface_src; BASE CLASS

  std::vector<std::vector<double>> Amat;
  std::vector<std::vector<double>> Atemp;
  std::vector<std::vector<double>> b;
  std::vector<double>              source;

  int LOCAL;
  double test_source;
  std::vector<double> test_mg_src;
  std::vector<double> zero_mg_src;





public:
  //################################################## Constructor
  NPT_SWEEP_PWL_SLAB(chi_mesh::MeshContinuum* vol_continuum,
                             CHI_DISCRETIZATION_PWL* discretization,
                             std::vector<NPT_CELLVIEW*>* cell_transport_views,
                             std::vector<double>* destination_phi,
                             std::vector<double>* source_moments,
                             NPT_GROUPSET* in_groupset,
                             TCrossSections* in_xsections,
                             int in_num_moms,
                             int in_max_cell_dofs)
  {
    grid_view           = vol_continuum;
    grid_fe_view        = discretization;
    grid_transport_view = cell_transport_views;
    x                   = destination_phi;
    q_moments           = source_moments;
    groupset            = in_groupset;
    xsections           = in_xsections;
    num_moms            = in_num_moms;
    max_cell_dofs       = in_max_cell_dofs;


    G                   = in_groupset->groups.size();

    a_and_b_initialized = false;
    suppress_surface_src= false;

    LOCAL = chi_mpi.location_id;

    test_source = 100.0/4.0/M_PI;
    test_mg_src.resize(G,test_source);
    test_mg_src[0] = test_source;
    zero_mg_src.resize(G,0.0);
  }


  //############################################################ Actual chunk
  void Sweep(chi_mesh::SweepManagement::AngleSet* angle_set)
  {
    int outface_master_counter=0;

    if (!a_and_b_initialized)
    {
      Amat.resize(max_cell_dofs,std::vector<double>(max_cell_dofs));
      Atemp.resize(max_cell_dofs,std::vector<double>(max_cell_dofs));
      b.resize(G,std::vector<double>(max_cell_dofs,0.0));
      source.resize(max_cell_dofs,0.0);

      a_and_b_initialized = true;
    }

    chi_mesh::SweepManagement::SPDS* spds = angle_set->GetSPDS();
    chi_mesh::SweepManagement::FLUDS* fluds = angle_set->fluds;

    GsSubSet& subset = groupset->grp_subsets[angle_set->ref_subset];
    int gs_ss_size  = groupset->grp_subset_sizes[angle_set->ref_subset];

    int gs_ss_begin = subset.first;
    int gs_ss_end   = subset.second;

    //Groupset subset first group number
    int gs_gi = groupset->groups[gs_ss_begin]->id;


    int deploc_face_counter = -1;
    int preloc_face_counter = -1;
    int bndry_face_counter  = -1;

    double* phi        = x->data();
    double* psi        = zero_mg_src.data();
    double* q_mom      = q_moments->data();


    //========================================================== Loop over each cell
    size_t num_loc_cells = spds->spls->item_id.size();
    for (int cr_i=0; cr_i<num_loc_cells; cr_i++)
    {
      int    cell_g_index = spds->spls->item_id[cr_i];
      auto   cell         = grid_view->cells[cell_g_index];

      chi_mesh::CellSlab* slab_cell =
        (chi_mesh::CellSlab*)cell;
      SlabFEView* cell_fe_view =
        (SlabFEView*)grid_fe_view->MapFeView(cell_g_index);
      NPT_CELLVIEW_FULL* transport_view =
        (NPT_CELLVIEW_FULL*)(*grid_transport_view)[slab_cell->cell_local_id];

      int     cell_dofs    = cell_fe_view->dofs;
      int     xs_id        = transport_view->xs_id;
      double* sigma_tg = (*xsections)[xs_id]->sigma_tg.data();


      //=================================================== Get Cell matrices
      const std::vector<std::vector<chi_mesh::Vector>>& L =
        cell_fe_view->IntV_shapeI_gradshapeJ;

      const std::vector<std::vector<double>>& M =
        cell_fe_view->IntV_shapeI_shapeJ;

      const std::vector<std::vector<double *>>& N =
        cell_fe_view->IntS_shapeI_shapeJ;

      //=================================================== Loop over angles in set
      int ni_deploc_face_counter = deploc_face_counter;
      int ni_preloc_face_counter = preloc_face_counter;
      int ni_bndry_face_counter  = bndry_face_counter;
      for (int n=0; n<angle_set->angles.size(); n++)
      {

        deploc_face_counter = ni_deploc_face_counter;
        preloc_face_counter = ni_preloc_face_counter;
        bndry_face_counter  = ni_bndry_face_counter;

        angle_num = angle_set->angles[n];
        omega = *groupset->quadrature->omegas[angle_num];
        wn    = groupset->quadrature->weights[angle_num];

        //============================================ Gradient matrix
        for (int i=0; i<cell_dofs; i++)
        {
          for (int j=0; j<cell_dofs; j++)
          {
            Amat[i][j] = omega.Dot(L[i][j]);
          }//for j
        }//for i

        for (int gsg=0; gsg<gs_ss_size; gsg++)
          b[gsg].assign(cell_dofs,0.0);

        //============================================ Surface integrals
        int num_faces = 2;
        int in_face_counter=-1;
        int internal_face_bndry_counter = -1;
        for (int f=0; f<num_faces; f++)
        {
          double mu       = omega.Dot(slab_cell->face_normals[f]);
          int    face_neighbor = transport_view->face_f_adj_part_id[f];

          //============================= Set flags
          if (mu>=0.0)
            transport_view->face_f_upwind_flag[f] = false;
          else
            transport_view->face_f_upwind_flag[f] = true;

          //This counter update-logic is for mapping an incident boundary
          //condition. Because it is cheap, the cell faces was mapped to a
          //corresponding boundary during initialization and is
          //independent of angle. Accessing things like reflective boundary
          //angular fluxes (and complex boundary conditions), requires the
          //more general bndry_face_counter.
          int bndry_map = -1;
          if (face_neighbor<0)
          {
            internal_face_bndry_counter++;
            bndry_map = transport_view->
              face_boundary_id[internal_face_bndry_counter];}

          if (mu < 0.0) //UPWIND
          {
            //============================== Increment face counters

            if (face_neighbor == LOCAL)
            {in_face_counter++;}

            if ((face_neighbor != LOCAL) && (face_neighbor>=0))
            {preloc_face_counter++;}

            if (face_neighbor<0)
            {bndry_face_counter++;}


            //============================== Loop over face vertices
            for (int fi=0; fi<1; fi++)
            {
              int i = f;
              //=========== Loop over face unknowns
              for (int fj=0; fj<1; fj++)
              {
                int j = f;

                // %%%%% LOCAL CELL DEPENDENCY %%%%%
                if (face_neighbor == LOCAL)
                {
                  psi = fluds->UpwindPsi(cr_i,in_face_counter,fj,0,n);
                }
                  // %%%%% NON-LOCAL CELL DEPENDENCY %%%%%
                else if (face_neighbor >= 0)
                {
                  psi = fluds->NLUpwindPsi(preloc_face_counter,fj,0,n);
                }
                  // %%%%% BOUNDARY CELL DEPENDENCY %%%%%
                else if (!suppress_surface_src)
                {
                  psi = angle_set->PsiBndry(bndry_face_counter,
                                            bndry_map,fj,gs_gi,n);
                }
                else
                {psi = zero_mg_src.data();}

                double mu_Nij = -mu*N[f][i][j];

                Amat[i][j] += mu_Nij;

                for (int gsg=0; gsg<gs_ss_size; gsg++)
                  b[gsg][i] += psi[gsg]*mu_Nij;
              }
            };

          }//if mu<0.0
        }//for f

        //========================================== Looping over groups
        double sigma_tgr = 0.0;
        double temp_src = 0.0;
        int gi_deploc_face_counter = deploc_face_counter;
        int gi_preloc_face_counter = preloc_face_counter;
        for (int gsg=0; gsg<gs_ss_size; gsg++)
        {
          deploc_face_counter = gi_deploc_face_counter;
          preloc_face_counter = gi_preloc_face_counter;

          g = gs_gi+gsg;

          //============================= Contribute source moments
          double m2d = 0.0;
          for (int i=0; i<cell_fe_view->dofs; i++)
          {
            temp_src = 0.0;
            for (int m=0; m<num_moms; m++)
            {
              m2d = groupset->m2d_op[m][angle_num];
              int ir = transport_view->MapDOF(i,m,g);
              temp_src += m2d*q_mom[ir];
            }
            source[i] = temp_src;
          }

          //============================= Mass Matrix and Source
          sigma_tgr = sigma_tg[g];
          for (int i=0; i<cell_fe_view->dofs; i++)
          {
            double temp = 0.0;
            for (int j=0; j<cell_fe_view->dofs; j++)
            {
              double Mij = M[i][j];
              Atemp[i][j] = Amat[i][j] + Mij*sigma_tgr;
              temp += Mij*source[j];
            }//for j
            b[gsg][i] += temp;
          }//for i


          //============================= Solve system
          chi_math_handler.GaussElimination(Atemp,b[gsg],cell_fe_view->dofs);


        }//for g


        //============================= Accumulate flux
        double wn_d2m = 0.0;
        for (int m=0; m<num_moms; m++)
        {
          wn_d2m = groupset->d2m_op[m][angle_num];
          for (int i=0; i<cell_fe_view->dofs; i++)
          {
            int ir = transport_view->MapDOF(i,m,gs_gi);
            for (int gsg=0; gsg<gs_ss_size; gsg++)
            {
              phi[ir+gsg] += wn_d2m*b[gsg][i];
            }
          }
        }

        //============================================= Outgoing fluxes
        int out_face_counter=-1;
        for (int f=0; f<num_faces; f++)
        {
          int     face_neighbor = transport_view->face_f_adj_part_id[f];
          bool    face_incident = transport_view->face_f_upwind_flag[f];
          double* psi = zero_mg_src.data();
          int num_face_indices = 1;

          //============================= Set flags and counters
          if (face_incident) continue;
          else out_face_counter++;

          //============================= Store outgoing Psi Locally
          if ((face_neighbor == LOCAL) && (!face_incident))
          {
            for (int fi=0; fi<num_face_indices; fi++)
            {
              int i = f;
              psi = fluds->OutgoingPsi(cr_i,out_face_counter,fi,n);

              for (int gsg=0; gsg<gs_ss_size; gsg++)
                psi[gsg] = b[gsg][i];
            }
          }//
          //============================= Store outgoing Psi Non-Locally
          else if ((face_neighbor != LOCAL) &&
                   (face_neighbor >= 0) &&
                   (!face_incident))
          {
            deploc_face_counter++;
            for (int fi=0; fi<num_face_indices; fi++)
            {
              int i = f;
              psi = fluds->NLOutgoingPsi(deploc_face_counter,fi,n);

              for (int gsg=0; gsg<gs_ss_size; gsg++)
                psi[gsg] = b[gsg][i];
            }//for fdof
          }//if non-local
        }//for f

      }//for n

    }// for cell

  }//Sweep function
};//class def

#endif