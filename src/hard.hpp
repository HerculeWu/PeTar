#pragma once
#ifdef USE_INTRINSIC_FOR_X86
#include<immintrin.h>
#endif

#include"cstdlib"
#include <algorithm>

#include"AR/symplectic_integrator.h"
#include"Hermite/hermite_integrator.h"
#include"Hermite/hermite_particle.h"
#include"hard_ptcl.hpp"
#include"soft_ptcl.hpp"
#include"hermite_interaction.hpp"
#include"hermite_information.hpp"
#include"hermite_perturber.hpp"
#include"ar_interaction.hpp"
#include"ar_perturber.hpp"
#include"search_group.hpp"
#include"artificial_particles.hpp"

//! Hard integrator parameter manager
class HardManager{
public:
    PS::F64 energy_error_max;
    PS::F64 eps_sq;
    ArtificialParticleManager ap_manager;
    H4::HermiteManager<HermiteInteraction> h4_manager;
    AR::SymplecticManager<ARInteraction> ar_manager;

    //! constructor
    HardManager(): energy_error_max(-1.0), eps_sq(-1.0), ap_manager(), h4_manager(), ar_manager() {}
    
    //! set softening
    void setEpsSq(const PS::F64 _eps_sq) {
        eps_sq = _eps_sq;
        h4_manager.interaction.eps_sq = _eps_sq;
        ar_manager.interaction.eps_sq = _eps_sq;
    }

    //! set gravitational constant
    void setG(const PS::F64 _g) {
        ap_manager.G = _g;
        h4_manager.interaction.G = _g;
        ar_manager.interaction.G = _g;
    }

    //! set time step range
    void setDtRange(const PS::F64 _dt_max, const PS::S32 _dt_min_index) {
        h4_manager.step.setDtRange(_dt_max, _dt_min_index);
        ar_manager.time_step_real_min = h4_manager.step.getDtMin();
        ar_manager.time_error_max_real = 0.25*ar_manager.time_step_real_min;
    }

    //! check paramters
    bool checkParams() {
        ASSERT(energy_error_max>0.0);
        ASSERT(eps_sq>=0.0);
        ASSERT(ap_manager.checkParams());
        ASSERT(h4_manager.checkParams());
        ASSERT(ar_manager.checkParams());
        return true;
    }

    //! write class data to file with binary format
    /*! @param[in] _fp: FILE type file for output
     */
    void writeBinary(FILE *_fp) {
        size_t size = sizeof(*this) - sizeof(ap_manager) - sizeof(h4_manager) - sizeof(ar_manager);
        fwrite(this, size, 1, _fp);
        ap_manager.writeBinary(_fp);
        h4_manager.writeBinary(_fp);
        ar_manager.writeBinary(_fp);
    }

    //! read class data to file with binary format
    /*! @param[in] _fp: FILE type file for reading
     */
    void readBinary(FILE *_fin) {
        size_t size = sizeof(*this) - sizeof(ap_manager) - sizeof(h4_manager) - sizeof(ar_manager);
        size_t rcount = fread(this, size, 1, _fin);
        if (rcount<1) {
            std::cerr<<"Error: Data reading fails! requiring data number is 1, only obtain "<<rcount<<".\n";
            abort();
        }
        ap_manager.readBinary(_fin);
        h4_manager.readBinary(_fin);
        ar_manager.readBinary(_fin);
    }

    //! print parameters
    void print(std::ostream & _fout) const{
        _fout<<"energy_error_max : "<<energy_error_max<<std::endl
             <<"eps_sq           : "<<eps_sq<<std::endl;
        ap_manager.print(_fout);
        h4_manager.print(_fout);
        ar_manager.print(_fout);
    }
};

struct HardEnergy{
    PS::F64 de;                 // energy error
    PS::F64 de_sd;              // slowdown energy error
    PS::F64 de_sd_change_cum;   // cumulative Etot_SD change due to the change of slowdown factor
    PS::F64 ekin_sd_correction; // correction from Ekin to Etot_sd
    PS::F64 epot_sd_correction; // correction from Epot to Etot_sd

    HardEnergy() {clear();}

    void clear() {
        de = de_sd = de_sd_change_cum = ekin_sd_correction = epot_sd_correction = 0.0;
    }

    void resetEnergyCorrection() {
        ekin_sd_correction = epot_sd_correction = 0.0;
    }

    HardEnergy& operator +=(const HardEnergy& _energy) {
        de    += _energy.de;
        de_sd += _energy.de_sd;
        de_sd_change_cum   += _energy.de_sd_change_cum;
        ekin_sd_correction += _energy.ekin_sd_correction;
        epot_sd_correction += _energy.epot_sd_correction;
        return *this;
    }

};

//! Hard system
class SystemHard{
private:
    typedef H4::ParticleH4<PtclHard> PtclH4;
    // Notice: if new variables added, change pardump also
    PS::F64 time_origin_;
    
    PS::ReallocatableArray<PtclH4> ptcl_hard_;
    PS::ReallocatableArray<PS::S32> n_ptcl_in_cluster_;
    PS::ReallocatableArray<PS::S32> n_ptcl_in_cluster_disp_;
    PS::ReallocatableArray<PS::S32> n_group_in_cluster_;
    PS::ReallocatableArray<PS::S32> n_group_in_cluster_offset_;
    PS::ReallocatableArray<PS::S32> adr_first_ptcl_arti_in_cluster_;
    PS::ReallocatableArray<PS::S32> i_cluster_changeover_update_;
    PS::S32 n_group_member_remote_; // number of members in groups but in remote nodes

    struct OPLessIDCluster{
        template<class T> bool operator() (const T & left, const T & right) const {
            return left.id_cluster < right.id_cluster;
        }
    };

public:
    HardManager* manager;

#ifdef PROFILE
    PS::S64 ARC_substep_sum;
    PS::S64 ARC_tsyn_step_sum;
    PS::F64 ARC_n_groups;
    PS::S64 H4_step_sum;
#endif
#ifdef HARD_CHECK_ENERGY
    HardEnergy energy;
#endif

    //! check paramters
    bool checkParams() {
        ASSERT(manager!=NULL);
        ASSERT(manager->checkParams());
        return true;
    }

private:
    //! Find groups and create aritfical particles to sys
    /* @param[in,out] _sys: global particle system
       @param[in,out] _ptcl_local: local saved particle data (will be reordered due to the groups)
       @param[in]     _n_ptcl_in_cluster: number of particles in one cluster
       @param[in]     _n_ptcl_in_cluster_disp: boundar of particle cluster
       @param[out]    _n_group_in_cluster: number of groups in one cluster
       @param[out]    _n_group_in_cluster_offset: boundary of groups in _adr_first_ptcl_arti_in_cluster
       @param[out]    _adr_first_ptcl_arti_in_cluster: address of the first artificial particle in each groups
       @param[in]     _dt_tree: tree time step for calculating r_search
     */
    template<class Tsys, class Tptcl>
    void findGroupsAndCreateArtificialParticlesImpl(Tsys & _sys,
                                                   PtclH4* _ptcl_local,
                                                   PS::ReallocatableArray<PS::S32>& _n_ptcl_in_cluster,
                                                   PS::ReallocatableArray<PS::S32>& _n_ptcl_in_cluster_disp,
                                                   PS::ReallocatableArray<PS::S32>& _n_group_in_cluster,
                                                   PS::ReallocatableArray<PS::S32>& _n_group_in_cluster_offset,
                                                   PS::ReallocatableArray<PS::S32>& _adr_first_ptcl_arti_in_cluster,
                                                   const PS::F64 _dt_tree) {
        const PS::S32 n_cluster = _n_ptcl_in_cluster.size();
#ifdef HARD_DEBUG
        assert(n_cluster<ARRAY_ALLOW_LIMIT);
#endif        
        _n_group_in_cluster.resizeNoInitialize(n_cluster);
        n_group_member_remote_=0;

        const PS::S32 num_thread = PS::Comm::getNumberOfThread();
        PS::ReallocatableArray<PtclH4> ptcl_artificial[num_thread];
        auto& ap_manager = manager->ap_manager;

#pragma omp parallel for schedule(dynamic)
        for (PS::S32 i=0; i<n_cluster; i++){
            const PS::S32 ith = PS::Comm::getThreadNum();
            PtclH4* ptcl_in_cluster = _ptcl_local + _n_ptcl_in_cluster_disp[i];
            const PS::S32 n_ptcl = _n_ptcl_in_cluster[i];
            // reset status
            for(PS::S32 j=0; j<n_ptcl; j++) {
                // ensure both hard local and global system have reset status, otherwise singles in global system may have wrong status
                ptcl_in_cluster[j].status.d = 0;
                PS::S64 adr=ptcl_in_cluster[j].adr_org;
                if(adr>=0) _sys[adr].status.d = 0;
            }
            // search groups
            SearchGroup<PtclH4> group;
            // merge groups
            group.searchAndMerge(ptcl_in_cluster, n_ptcl);

            // generate artificial particles,
            ap_manager.createArtificialParticles(i, ptcl_in_cluster, n_ptcl, ptcl_artificial[ith], _n_group_in_cluster[i], group, _dt_tree);
        }

        // n_group_in_cluster_offset
        _n_group_in_cluster_offset.resizeNoInitialize(n_cluster+1);
        _n_group_in_cluster_offset[0] = 0;
        for (PS::S32 i=0; i<n_cluster; i++) 
            _n_group_in_cluster_offset[i+1] = _n_group_in_cluster_offset[i] + _n_group_in_cluster[i];
#ifdef HARD_DEBUG
        assert(_n_group_in_cluster_offset[n_cluster]<ARRAY_ALLOW_LIMIT);
#endif        
        _adr_first_ptcl_arti_in_cluster.resizeNoInitialize(_n_group_in_cluster_offset[n_cluster]);


        // add artificial particle to particle system
        PS::S32 rank = PS::Comm::getRank();
        // Get the address offset for new artificial ptcl array in each thread in _sys
        PS::S64 sys_ptcl_artificial_thread_offset[num_thread+1];
        PS::ReallocatableArray<PS::S32> i_cluster_changeover_update_threads[num_thread];
        sys_ptcl_artificial_thread_offset[0] = _sys.getNumberOfParticleLocal();
        for(PS::S32 i=0; i<num_thread; i++) {
            sys_ptcl_artificial_thread_offset[i+1] = sys_ptcl_artificial_thread_offset[i] + ptcl_artificial[i].size();
            i_cluster_changeover_update_threads[i].resizeNoInitialize(0);
        }
        _sys.setNumberOfParticleLocal(sys_ptcl_artificial_thread_offset[num_thread]);
        
        const PS::S32 n_artificial_per_group = ap_manager.getArtificialParticleN();
#pragma omp parallel for        
        for(PS::S32 i=0; i<num_thread; i++) {
            // ptcl_artificial should be integer times of 2*n_split+1
            assert(ptcl_artificial[i].size()%n_artificial_per_group==0);
            // Add particle to ptcl sys
            for (PS::S32 j=0; j<ptcl_artificial[i].size(); j++) {
                PS::S32 adr = j+sys_ptcl_artificial_thread_offset[i];
                ptcl_artificial[i][j].adr_org=adr;
                _sys[adr]=Tptcl(ptcl_artificial[i][j],rank,adr);
            }
            PS::S32 group_offset=0, j_group_recored=-1;
            // Update the status of group members to c.m. address in ptcl sys. Notice c.m. is at the end of an artificial particle group
            for (PS::S32 j=0; j<ptcl_artificial[i].size(); j+=n_artificial_per_group) {
                auto* pj = &ptcl_artificial[i][j];
                auto* pcm = ap_manager.getCMParticles(pj);
                PS::S32 n_members = ap_manager.getMemberN(pj);
                PS::S32 i_cluster = ap_manager.getClusterID(pj);
                PS::S32 j_group = ap_manager.getGroupID(pj);
                PS::F64 rsearch_cm = pcm->r_search;
                auto& changeover_cm= pcm->changeover;
#ifdef HARD_DEBUG
                assert(rsearch_cm>changeover_cm.getRout());
#endif                
                // make sure group index increase one by one
                assert(j_group==j_group_recored+1);
                j_group_recored=j_group;

                // changeover update flag
                bool changeover_update_flag=false;
                // update member status
                for (PS::S32 k=0; k<n_members; k++) {
                    PS::S32 kl = _n_ptcl_in_cluster_disp[i_cluster]+group_offset+k;
                    auto& p_loc = _ptcl_local[kl];
#ifdef HARD_DEBUG
                    // check whether ID is consistent.
                    if(k==0) assert(p_loc.id==ap_manager.getFirstMemberID(pj));
#endif
                    // save c.m. address 
                    p_loc.status.d = -pcm->adr_org;

                    // set changeover r_scale_next for updating changeover next tree step
                    // set rsearch to maximum of c.m. rsearch and member rsearch
                    PS::F64 rin_cm = changeover_cm.getRin();
                    PS::F64 rin_p  = p_loc.changeover.getRin();
                    if (rin_p!=rin_cm) {
                        // avoid round-off error case
                        if (abs(rin_p-rin_cm)<1e-10) {
                            p_loc.changeover = changeover_cm;
                        }
                        else {
                            p_loc.changeover.r_scale_next = changeover_cm.getRin()/p_loc.changeover.getRin();
                            p_loc.r_search = std::max(p_loc.r_search, rsearch_cm);
                            changeover_update_flag = true;
                        }
                    }
                    // shift mass to mass_bk
                    p_loc.mass_bk.d = p_loc.mass;
                    p_loc.mass = 0;
#ifdef HARD_DEBUG
                    assert(p_loc.mass_bk.d>0.0);
#endif
                    // also update global particle to be consistent
                    const PS::S64 p_glb_adr= p_loc.adr_org;
                    if(p_glb_adr>=0) {
                        auto& p_glb = _sys[p_glb_adr];
                        p_glb.mass      = p_loc.mass;
                        p_glb.status.d  = p_loc.status.d;
                        p_glb.mass_bk.d = p_loc.mass_bk.d;
                        p_glb.changeover= p_loc.changeover;
                        p_glb.r_search  = p_loc.r_search;
                    }
                    else {
                        // this is remoted member;
                        n_group_member_remote_++;
                    }

                }
                // record i_cluster if changeover change
                if (changeover_update_flag) i_cluster_changeover_update_threads[i].push_back(i_cluster);

                // shift cluster
                if(j_group==_n_group_in_cluster[i_cluster]-1) {
                    group_offset=0;
                    j_group_recored=-1;
                }
                else group_offset += n_members; // group offset in the ptcl list index of one cluster
                // j_group should be consistent with n_group[i_cluster];
                assert(j_group<=_n_group_in_cluster[i_cluster]);

                // save first address of artificial particle
                _adr_first_ptcl_arti_in_cluster[_n_group_in_cluster_offset[i_cluster]+j_group] = ptcl_artificial[i][j].adr_org;
            }
        }
        
        // merge i_cluster_changeover
        i_cluster_changeover_update_.resizeNoInitialize(0);
        for(PS::S32 i=0; i<num_thread; i++) {
            for (PS::S32 j=0; j<i_cluster_changeover_update_threads[i].size();j++)
                i_cluster_changeover_update_.push_back(i_cluster_changeover_update_threads[i][j]);
        }
        // sort data
        PS::S32 i_cluster_size = i_cluster_changeover_update_.size();
        if (i_cluster_size>0) {
            PS::S32* i_cluster_data = i_cluster_changeover_update_.getPointer();
            std::sort(i_cluster_data, i_cluster_data+i_cluster_size, [] (const PS::S32 &a, const PS::S32 &b) { return a<b; });
            // remove dup
            PS::S32* i_end = std::unique(i_cluster_data, i_cluster_data+i_cluster_size);
#ifdef HARD_DEBUG
            assert(i_end-i_cluster_data>=0&&i_end-i_cluster_data<=i_cluster_size);
            std::cerr<<"Changeover change cluster found: ";
            for (auto k=i_cluster_data; k<i_end; k++) {
                std::cerr<<*k<<" ";
            }
            std::cerr<<std::endl;
#endif
            i_cluster_changeover_update_.resizeNoInitialize(i_end-i_cluster_data);
        }
    }

    //! correct force and potential for soft force with changeover function
    /*!
      @param[in,out] _pi: particle for correction
      @param[in] _pj: j particle to calculate correction
     */
    template <class Tpi>
    inline void calcAccPotShortWithLinearCutoff(Tpi& _pi,
                                                const Ptcl& _pj) {
        const PS::F64vec dr = _pi.pos - _pj.pos;
        const PS::F64 dr2 = dr * dr;
        const PS::F64 dr2_eps = dr2 + manager->eps_sq;
        const PS::F64 drinv = 1.0/sqrt(dr2_eps);
        const PS::F64 movr = _pj.mass * drinv;
        const PS::F64 drinv2 = drinv * drinv;
        const PS::F64 movr3 = movr * drinv2;
        const PS::F64 dr_eps = drinv * dr2_eps;
        const PS::F64 k = 1.0 - ChangeOver::calcAcc0WTwo(_pi.changeover, _pj.changeover, dr_eps);

        // linear cutoff 
        const PS::F64 r_out = manager->ap_manager.r_out_base;
        const PS::F64 r_out2 = r_out * r_out;
        const PS::F64 dr2_max = (dr2_eps > r_out2) ? dr2_eps : r_out2;
        const PS::F64 drinv_max = 1.0/sqrt(dr2_max);
        const PS::F64 movr_max = _pj.mass * drinv_max;
        const PS::F64 drinv2_max = drinv_max*drinv_max;
        const PS::F64 movr3_max = movr_max * drinv2_max;

#ifdef ONLY_SOFT
        const PS::F64 kpot  = 1.0 - ChangeOver::calcPotWTwo(_pi.changeover, _pj.changeover, dr_eps);
        // single, remove linear cutoff, obtain changeover soft potential
        if (_pj.status.d==0) _pi.pot_tot -= dr2_eps>r_out2? 0.0: (movr*kpot  - movr_max);   
        // member, mass is zero, use backup mass
        else if (_pj.status.d<0) _pi.pot_tot -= dr2_eps>r_out2? 0.0: (_pj.mass_bk.d*drinv*kpot  - movr_max);   
        // (orbitial) artificial, should be excluded in potential calculation, since it is inside neighbor, movr_max cancel it to 0.0
        else _pi.pot_tot += movr_max; 
#else
        // single/member, remove linear cutoff, obtain total potential
        if (_pj.status.d==0) _pi.pot_tot -= (movr - movr_max);   
        // member, mass is zero, use backup mass
        else if (_pj.status.d<0) _pi.pot_tot -= (_pj.mass_bk.d*drinv  - movr_max);   
        // (orbitial) artificial, should be excluded in potential calculation, since it is inside neighbor, movr_max cancel it to 0.0
        else _pi.pot_tot += movr_max; 
#endif
        // correct to changeover soft acceleration
        _pi.acc -= (movr3*k - movr3_max)*dr;
    }

    //! correct force and potential for soft force with changeover function
    /*!
      @param[in,out] _pi: particle for correction
      @param[in] _pj: j particle to calculate correction
     */
    template <class Tpi>
    inline void calcAccPotShortWithLinearCutoff(Tpi& _pi,
                                                const EPJSoft& _pj) {
        const PS::F64vec dr = _pi.pos - _pj.pos;
        const PS::F64 dr2 = dr * dr;
        const PS::F64 dr2_eps = dr2 + manager->eps_sq;
        const PS::F64 r_out = manager->ap_manager.r_out_base;
        const PS::F64 r_out2 = r_out * r_out;
        const PS::F64 drinv = 1.0/sqrt(dr2_eps);
        const PS::F64 movr = _pj.mass * drinv;
        const PS::F64 drinv2 = drinv * drinv;
        const PS::F64 movr3 = movr * drinv2;
        const PS::F64 dr_eps = drinv * dr2_eps;
        ChangeOver chj;
        chj.setR(_pj.r_in, _pj.r_out);
        const PS::F64 k = 1.0 - ChangeOver::calcAcc0WTwo(_pi.changeover, chj, dr_eps);

        // linear cutoff 
        const PS::F64 dr2_max = (dr2_eps > r_out2) ? dr2_eps : r_out2;
        const PS::F64 drinv_max = 1.0/sqrt(dr2_max);
        const PS::F64 movr_max = _pj.mass * drinv_max;
        const PS::F64 drinv2_max = drinv_max*drinv_max;
        const PS::F64 movr3_max = movr_max * drinv2_max;

#ifdef ONLY_SOFT
        const PS::F64 kpot  = 1.0 - ChangeOver::calcPotWTwo(_pi.changeover, chj, dr_eps);
        // single, remove linear cutoff, obtain changeover soft potential
        if (_pj.status.d==0) _pi.pot_tot -= dr2_eps>r_out2? 0.0: (movr*kpot  - movr_max);   
        // member, mass is zero, use backup mass
        else if (_pj.status.d<0) _pi.pot_tot -= dr2_eps>r_out2? 0.0: (_pj.mass_bk.d*drinv*kpot  - movr_max);   
        // (orbitial) artificial, should be excluded in potential calculation, since it is inside neighbor, movr_max cancel it to 0.0
        else _pi.pot_tot += movr_max; 
#else
        // single/member, remove linear cutoff, obtain total potential
        if (_pj.status.d==0) _pi.pot_tot -= (movr - movr_max);   
        // member, mass is zero, use backup mass
        else if (_pj.status.d<0) _pi.pot_tot -= (_pj.mass_bk.d*drinv  - movr_max);   
        // (orbitial) artificial, should be excluded in potential calculation, since it is inside neighbor, movr_max cancel it to 0.0
        else _pi.pot_tot += movr_max; 
#endif
        // correct to changeover soft acceleration
        _pi.acc -= (movr3*k - movr3_max)*dr;
    }

    //! correct force and potential for changeover function change
    /*!
      @param[in,out] _pi: particle for correction
      @param[in] _pj: j particle to calculate correction
     */
    template <class Tpi>
    inline void calcAccChangeOverCorrection(Tpi& _pi,
                                            const Ptcl& _pj) {
        const PS::F64vec dr = _pi.pos - _pj.pos;
        const PS::F64 dr2 = dr * dr;
        const PS::F64 dr2_eps = dr2 + manager->eps_sq;
        const PS::F64 drinv = 1.0/sqrt(dr2_eps);
        const PS::F64 movr = _pj.mass * drinv;
        const PS::F64 drinv2 = drinv * drinv;
        const PS::F64 movr3 = movr * drinv2;
        const PS::F64 dr_eps = drinv * dr2_eps;

        // old
        const PS::F64 kold = 1.0 - ChangeOver::calcAcc0WTwo(_pi.changeover, _pj.changeover, dr_eps);

        // new
        ChangeOver chinew, chjnew;
        chinew.setR(_pi.changeover.getRin()*_pi.changeover.r_scale_next, _pi.changeover.getRout()*_pi.changeover.r_scale_next);
        chjnew.setR(_pj.changeover.getRin()*_pj.changeover.r_scale_next, _pj.changeover.getRout()*_pj.changeover.r_scale_next);
        const PS::F64 knew = 1.0 - ChangeOver::calcAcc0WTwo(chinew, chjnew, dr_eps);

        // correct to changeover soft acceleration
        _pi.acc -= movr3*(knew-kold)*dr;
    }

    //! correct force and potential for changeover function change
    /*!
      @param[in,out] _pi: particle for correction
      @param[in] _pj: j particle to calculate correction
     */
    template <class Tpi>
    inline void calcAccChangeOverCorrection(Tpi& _pi,
                                            const EPJSoft& _pj) {
        const PS::F64vec dr = _pi.pos - _pj.pos;
        const PS::F64 dr2 = dr * dr;
        const PS::F64 dr2_eps = dr2 + manager->eps_sq;
        const PS::F64 drinv = 1.0/sqrt(dr2_eps);
        const PS::F64 movr = _pj.mass * drinv;
        const PS::F64 drinv2 = drinv * drinv;
        const PS::F64 movr3 = movr * drinv2;
        const PS::F64 dr_eps = drinv * dr2_eps;

        ChangeOver chjold;
        chjold.setR(_pj.r_in, _pj.r_out);
        // old
        const PS::F64 kold = 1.0 - ChangeOver::calcAcc0WTwo(_pi.changeover, chjold, dr_eps);

        // new
        ChangeOver chinew, chjnew;
        chinew.setR(_pi.changeover.getRin()*_pi.changeover.r_scale_next, _pi.changeover.getRout()*_pi.changeover.r_scale_next);
        chjnew.setR(_pj.r_in*_pj.r_scale_next, _pj.r_out*_pj.r_scale_next);
        const PS::F64 knew = 1.0 - ChangeOver::calcAcc0WTwo(chinew, chjnew, dr_eps);

        // correct to changeover soft acceleration
        _pi.acc -= movr3*(knew-kold)*dr;
    }

#ifdef KDKDK_4TH
    template <class Tpi>
    inline void calcAcorrShortWithLinearCutoff(Tpi& _pi,
                                               const Ptcl& _pj) {
        const PS::F64 r_out = manager->changeover.getRout();
        const PS::F64 r_out2 = r_out * r_out;

        const PS::F64vec dr = _pi.pos - _pj.pos;
        const PS::F64vec da = _pi.acc - _pi.acc;
        const PS::F64 dr2 = dr * dr;
        const PS::F64 dr2_eps = dr2 + manager->eps_sq;
        const PS::F64 drda = dr*da;
        const PS::F64 drinv = 1.0/sqrt(dr2_eps);
        const PS::F64 movr = _pj.mass * drinv;
        const PS::F64 drinv2 = drinv * drinv;
        const PS::F64 movr3 = movr * drinv2;
        const PS::F64 dr_eps = drinv * dr2_eps;

        const PS::F64 k = 1.0 - ChangeOver::calcAcc0WTwo(_pi.changeover, _pj.changeover, dr_eps);
        const PS::F64 kdot = - ChangeOver::calcAcc1WTwo(_pi.changeover, _pj.changeover, dr_eps);

        const PS::F64 dr2_max = (dr2_eps > r_out2) ? dr2_eps : r_out2;
        const PS::F64 drinv_max = 1.0/sqrt(dr2_max);
        const PS::F64 movr_max = _pj.mass * drinv_max;
        const PS::F64 drinv2_max = drinv_max*drinv_max;
        const PS::F64 movr3_max = movr_max * drinv2_max;

        const PS::F64 alpha = drda*drinv2;
        const PS::F64 alpha_max = drda * drinv2_max;
        const PS::F64vec acorr_k = movr3 * (k*da - (3.0*k*alpha - kdot) * dr);
        const PS::F64vec acorr_max = movr3_max * (da - 3.0*alpha_max * dr);

        _pi.acorr -= 2.0 * (acorr_k - acorr_max);
        //acci + dt_kick * dt_kick * acorri /48; 
    }

    template <class Tpi>
    inline void calcAcorrShortWithLinearCutoff(Tpi& _pi,
                                               const EPJSoft& _pj) {
        const PS::F64 r_out = manager->changeover.getRout();
        const PS::F64 r_out2 = r_out * r_out;

        const PS::F64vec dr = _pi.pos - _pj.pos;
        const PS::F64vec da = _pi.acc - _pi.acc;
        const PS::F64 dr2 = dr * dr;
        const PS::F64 dr2_eps = dr2 + manager->eps_sq;
        const PS::F64 drda = dr*da;
        const PS::F64 drinv = 1.0/sqrt(dr2_eps);
        const PS::F64 movr = _pj.mass * drinv;
        const PS::F64 drinv2 = drinv * drinv;
        const PS::F64 movr3 = movr * drinv2;
        const PS::F64 dr_eps = drinv * dr2_eps;
        ChangeOver chj;
        chj.setR(_pj.r_in, _pj.r_out);
        const PS::F64 k = 1.0 - ChangeOver::calcAcc0WTwo(_pi.changeover, chj, dr_eps);
        const PS::F64 kdot = - ChangeOver::calcAcc1WTwo(_pi.changeover, chj, dr_eps);

        const PS::F64 dr2_max = (dr2_eps > r_out2) ? dr2_eps : r_out2;
        const PS::F64 drinv_max = 1.0/sqrt(dr2_max);
        const PS::F64 movr_max = _pj.mass * drinv_max;
        const PS::F64 drinv2_max = drinv_max*drinv_max;
        const PS::F64 movr3_max = movr_max * drinv2_max;

        const PS::F64 alpha = drda*drinv2;
        const PS::F64 alpha_max = drda * drinv2_max;
        const PS::F64vec acorr_k = movr3 * (k*da - (3.0*k*alpha - kdot) * dr);
        const PS::F64vec acorr_max = movr3_max * (da - 3.0*alpha_max * dr);

        _pi.acorr -= 2.0 * (acorr_k - acorr_max);
        //acci + dt_kick * dt_kick * acorri /48; 
    }
#endif

    //! soft force correction use tree neighbor search for one particle
    /*
      @param[in,out] _psoft: particle in global system need to be corrected for acc and pot
      @param[in] _tree: tree for force
      @param[in] _acorr_flag: flag to do acorr for KDKDK_4TH case
     */
    template <class Tpsoft, class Ttree, class Tepj>
    void correctForceWithCutoffTreeNeighborOneParticleImp(Tpsoft& _psoft, 
                                                          Ttree& _tree,
                                                          const bool _acorr_flag=false) {
        Tepj * ptcl_nb = NULL;
        PS::S32 n_ngb = _tree.getNeighborListOneParticle(_psoft, ptcl_nb);
#ifdef HARD_DEBUG
        assert(n_ngb >= 1);
#endif
        // self-potential correction 
        // no correction for orbital artificial particles because the potential are not used for any purpose
        // no correction for member particles because their mass is zero during the soft force calculation, the self-potential contribution is also zero.
        if (_psoft.status.d==0) _psoft.pot_tot += _psoft.mass/manager->ap_manager.r_out_base; // single

        // loop neighbors
        for(PS::S32 k=0; k<n_ngb; k++){
            if (ptcl_nb[k].id == _psoft.id) continue;

#ifdef KDKDK_4TH
            if(_acorr_flag) 
                calcAcorrShortWithLinearCutoff(_psoft, ptcl_nb[k]);
            else
#endif
                calcAccPotShortWithLinearCutoff(_psoft, ptcl_nb[k]);
        }
    }

    //! soft force correction for artificial particles in one cluster
    /* 1. Correct cutoff for artificial particles
       2. The c.m. force is substracted from tidal tensor force
       3. c.m. force is replaced by the averaged force on orbital particles
       @param[in,out] _sys: global particle system, acc is updated
       @param[in] _ptcl_local: particle in systme_hard
       @param[in] _adr_real_start: real particle start address in _ptcl_local
       @param[in] _adr_real_end:   real particle end (+1) address in _ptcl_local
       @param[in] _n_group:  number of groups in cluster
       @param[in] _adr_first_ptcl_arti_in_cluster: address of the first artificial particle in each groups
       @param[in] _acorr_flag: flag to do acorr for KDKDK_4TH case
     */
    template <class Tsys>
    void correctForceWithCutoffArtificialOneClusterImp(Tsys& _sys, 
                                                      const PtclH4* _ptcl_local,
                                                      const PS::S32 _adr_real_start,
                                                      const PS::S32 _adr_real_end,
                                                      const PS::S32 _n_group,
                                                      const PS::S32* _adr_first_ptcl_arti_in_cluster,
                                                      const bool _acorr_flag) {

        auto& ap_manager = manager->ap_manager;
        for (int j=0; j<_n_group; j++) {  // j: j_group
            PS::S32 j_start = _adr_first_ptcl_arti_in_cluster[j];
            auto* pj = &(_sys[j_start]);

            // loop all artificial particles: tidal tensor, orbital and c.m. particle
            for (int k=0; k<ap_manager.getArtificialParticleN(); k++) {  
                // k: k_ptcl_arti

                // loop orbital artificial particle
                // group
                for (int kj=0; kj<_n_group; kj++) { // group
                    PS::S32 kj_start = _adr_first_ptcl_arti_in_cluster[kj];
                    auto* porb_kj = ap_manager.getOrbitalParticles(&_sys[kj_start]);

                    // particle arti orbital
                    for (int kk=0; kk<ap_manager.getOrbitalParticleN(); kk++) {
                        if(&porb_kj[kk]==&(pj[k])) continue; //avoid same particle
#ifdef KDKDK_4TH
                        if(_acorr_flag) 
                            calcAcorrShortWithLinearCutoff(pj[k], porb_kj[kk]);
                        else
#endif
                            calcAccPotShortWithLinearCutoff(pj[k], porb_kj[kk]);
                    }
                }

                // loop real particle
                for (int kj=_adr_real_start; kj<_adr_real_end; kj++) {
#ifdef KDKDK_4TH
                    if(_acorr_flag) {
                        PS::S64 adr_kj = _ptcl_local[kj].adr_org;
                        calcAcorrShortWithLinearCutoff(pj[k], _sys[adr_kj]);
                    }
                    else
#endif
                        calcAccPotShortWithLinearCutoff(pj[k], _ptcl_local[kj]);
                }
            }
            
            ap_manager.correctArtficialParticleForce(pj);
        }
    }

    //! Soft force correction due to different cut-off function
    /* Use cluster member
       1. first correct for artificial particles, then for cluster member. 
       2. The c.m. force is substracted from tidal tensor force
       3. c.m. force is replaced by the averaged force on orbital particles

       @param[in,out] _sys: global particle system, acc is updated
       @param[in] _ptcl_local: particle in systme_hard
       @param[in] _n_ptcl_in_cluster: number of particles in clusters
       @param[in] _n_ptcl_in_cluster_offset: boundary of clusters in _adr_sys_in_cluster
       @parma[in] _n_group_in_cluster: number of groups in clusters
       @param[in] _n_group_in_cluster_offset: boundary of groups in _adr_first_ptcl_arti_in_cluster
       @param[in] _adr_first_ptcl_arti_in_cluster: address of the first artificial particle in each groups
       @param[in] _acorr_flag: flag to do acorr for KDKDK_4TH case
    */
    template <class Tsys>
    void correctForceWithCutoffClusterImp(Tsys& _sys, 
                                          const PtclH4* _ptcl_local,
                                          const PS::ReallocatableArray<PS::S32>& _n_ptcl_in_cluster,
                                          const PS::ReallocatableArray<PS::S32>& _n_ptcl_in_cluster_offset,
                                          const PS::ReallocatableArray<PS::S32>& _n_group_in_cluster,
                                          const PS::ReallocatableArray<PS::S32>& _n_group_in_cluster_offset,
                                          const PS::ReallocatableArray<PS::S32>& _adr_first_ptcl_arti_in_cluster,
                                          const bool _acorr_flag) {
        const PS::S32 n_cluster = _n_ptcl_in_cluster.size();
        auto& ap_manager = manager->ap_manager;
#pragma omp parallel for schedule(dynamic)
        for (int i=0; i<n_cluster; i++) {  // i: i_cluster
            PS::S32 adr_real_start= _n_ptcl_in_cluster_offset[i];
            PS::S32 adr_real_end= _n_ptcl_in_cluster_offset[i+1];
            // artificial particle group number
            PS::S32 n_group = _n_group_in_cluster[i];
            //PS::S32 n_group_offset = _n_group_in_cluster_offset[i];
            const PS::S32* adr_first_ptcl_arti = n_group>0? &_adr_first_ptcl_arti_in_cluster[_n_group_in_cluster_offset[i]] : NULL;

            // correction for artificial particles
            correctForceWithCutoffArtificialOneClusterImp(_sys, _ptcl_local, adr_real_start, adr_real_end, n_group, adr_first_ptcl_arti, _acorr_flag);

            // obtain correction for real particles in clusters
            for (int j=adr_real_start; j<adr_real_end; j++) {
                PS::S64 adr = _ptcl_local[j].adr_org;
#ifdef HARD_DEBUG
                assert(_sys[adr].id==_ptcl_local[j].id);
#endif
                //self-potential correction for non-group member, group member has mass zero, so no need correction
                if(_sys[adr].status.d==0) _sys[adr].pot_tot += _sys[adr].mass/manager->ap_manager.r_out_base;

                // cluster member
                for (int k=adr_real_start; k<adr_real_end; k++) {
                    if(k==j) continue;
#ifdef KDKDK_4TH
                    if(_acorr_flag) {
                        PS::S64 adr_k = _ptcl_local[k].adr_org;
                        calcAcorrShortWithLinearCutoff(_sys[adr], _sys[adr_k]);
                    }
                    else
#endif
                        calcAccPotShortWithLinearCutoff(_sys[adr], _ptcl_local[k]);
                }

                // orbital artificial particle
                for (int k=0; k<n_group; k++) {
                    // loop artificial particle orbital
                    PS::S32 k_start = adr_first_ptcl_arti[k];
                    auto* porb_k = ap_manager.getOrbitalParticles(&(_sys[k_start]));
                    for (int ki=0; ki<ap_manager.getOrbitalParticleN(); ki++) {
#ifdef KDKDK_4TH
                        if(_acorr_flag) 
                            calcAcorrShortWithLinearCutoff(_sys[adr], porb_k[ki]);
                        else
#endif
                            calcAccPotShortWithLinearCutoff(_sys[adr], porb_k[ki]);
                    }
                }
            
            }
        }
    }

//! Soft force correction due to different cut-off function
/* Use tree neighbor search
   1. first correct for artificial particles use cluster information, 
   2. The c.m. force is substracted from tidal tensor force
   3. c.m. force is replaced by the averaged force on orbital particles
   4. then use tree neighbor search for local cluster real member. 
   @param[in] _sys: global particle system, acc is updated
   @param[in] _tree: tree for force
   @param[in] _ptcl_local: particle in systme_hard
   @param[in] _n_ptcl_in_cluster: number of particles in clusters
   @param[in] _n_ptcl_in_cluster_offset: boundary of clusters in _adr_sys_in_cluster
   @parma[in] _n_group_in_cluster: number of groups in clusters
   @param[in] _n_group_in_cluster_offset: boundary of groups in _adr_first_ptcl_arti_in_cluster
   @param[in] _adr_first_ptcl_arti_in_cluster: address of the first artificial particle in each groups
   @param[in] _adr_send: particle in sending list of connected clusters
   @param[in] _acorr_flag: flag to do acorr for KDKDK_4TH case
*/
    template <class Tsys, class Tpsoft, class Ttree, class Tepj>
    void correctForceWithCutoffTreeNeighborAndClusterImp(Tsys& _sys,
                                                         Ttree& _tree,
                                                         const PtclH4* _ptcl_local,
                                                         const PS::ReallocatableArray<PS::S32>& _n_ptcl_in_cluster,
                                                         const PS::ReallocatableArray<PS::S32>& _n_ptcl_in_cluster_offset,
                                                         const PS::ReallocatableArray<PS::S32>& _n_group_in_cluster,
                                                         const PS::ReallocatableArray<PS::S32>& _n_group_in_cluster_offset,
                                                         const PS::ReallocatableArray<PS::S32>& _adr_first_ptcl_arti_in_cluster,
                                                         const PS::ReallocatableArray<PS::S32>& _adr_send,
                                                         const bool _acorr_flag=false) {

        const PS::S32 n_cluster = _n_ptcl_in_cluster.size();

#pragma omp parallel for schedule(dynamic)
        for (int i=0; i<n_cluster; i++) {  // i: i_cluster
            PS::S32 adr_real_start= _n_ptcl_in_cluster_offset[i];
            PS::S32 adr_real_end= _n_ptcl_in_cluster_offset[i+1];
            // artificial particle group number
            PS::S32 n_group = _n_group_in_cluster[i];
            //PS::S32 n_group_offset = _n_group_in_cluster_offset[i];
            const PS::S32* adr_first_ptcl_arti = &_adr_first_ptcl_arti_in_cluster[_n_group_in_cluster_offset[i]];

            // correction for artificial particles
            correctForceWithCutoffArtificialOneClusterImp(_sys, _ptcl_local, adr_real_start, adr_real_end, n_group, adr_first_ptcl_arti, _acorr_flag);

            // obtain correction for real particles in clusters use tree neighbor search
            for (int j=adr_real_start; j<adr_real_end; j++) {
                PS::S64 adr = _ptcl_local[j].adr_org;
                // only do for local particles
#ifdef HARD_DEBUG
                if(adr>=0) assert(_sys[adr].id==_ptcl_local[j].id);
#endif
                if(adr>=0) correctForceWithCutoffTreeNeighborOneParticleImp<Tpsoft, Ttree, Tepj>(_sys[adr], _tree, _acorr_flag);
            }
        }

        const PS::S32 n_send = _adr_send.size();
#pragma omp parallel for 
        // sending list to other nodes need also be corrected.
        for (int i=0; i<n_send; i++) {
            PS::S64 adr = _adr_send[i];
            correctForceWithCutoffTreeNeighborOneParticleImp<Tpsoft, Ttree, Tepj>(_sys[adr], _tree, _acorr_flag); 
        }
    }
    
    //! soft force correction completely use tree neighbor search
    /* @param[in,out] _sys: global particle system, acc is updated
       @param[in] _tree: tree for force
       @param[in] _ptcl_local: particle in systme_hard, only used to get adr_org
       @param[in] _n_ptcl: total number of particles in all clusters
       @param[in] _adr_ptcl_artificial_start: start address of artificial particle in _sys
       @param[in] _acorr_flag: flag to do acorr for KDKDK_4TH case
    */
    template <class Tsys, class Tpsoft, class Ttree, class Tepj>
    void correctForceWithCutoffTreeNeighborImp(Tsys& _sys, 
                                               Ttree& _tree, 
                                               const PtclH4* _ptcl_local,
                                               const PS::S32 _n_ptcl,
                                               const PS::S32 _adr_ptcl_artificial_start,
                                               const bool _acorr_flag=false) { 
        // for real particle
#pragma omp parallel for schedule(dynamic)
        for (int i=0; i<_n_ptcl; i++) {
            PS::S64 adr = _ptcl_local[i].adr_org;
            if(adr>=0) correctForceWithCutoffTreeNeighborOneParticleImp<Tpsoft, Ttree, Tepj>(_sys[adr], _tree, _acorr_flag);
        }

        // for artificial particle
        const PS::S32 n_tot = _sys.getNumberOfParticleLocal();
#pragma omp parallel for schedule(dynamic)
        for (int i=_adr_ptcl_artificial_start; i<n_tot; i++) 
            correctForceWithCutoffTreeNeighborOneParticleImp<Tpsoft, Ttree, Tepj>(_sys[i], _tree, _acorr_flag);

        auto& ap_manager = manager->ap_manager;
        const PS::S32 n_artificial_per_group = ap_manager.getArtificialParticleN();
#ifdef HARD_DEBUG
        assert((n_tot-_adr_ptcl_artificial_start)%n_artificial_per_group==0);
#endif
#pragma omp parallel for schedule(dynamic)
        for (int i=_adr_ptcl_artificial_start; i<n_tot; i+=n_artificial_per_group)
            ap_manager.correctArtficialParticleForce(&(_sys[i]));

    }

    //! soft force correction completely use tree neighbor search for all particles
    /* @param[in,out] _sys: global particle system, acc is updated
       @param[in] _tree: tree for force
       @param[in] _adr_ptcl_artificial_start: start address of artificial particle in _sys
    */
    template <class Tsys, class Tpsoft, class Ttree, class Tepj>
    void correctForceWithCutoffTreeNeighborImp(Tsys& _sys, 
                                               Ttree& _tree, 
                                               const PS::S32 _adr_ptcl_artificial_start,
                                               const bool _acorr_flag=false) {
        // for artificial particle
        const PS::S32 n_tot = _sys.getNumberOfParticleLocal();

#pragma omp parallel for schedule(dynamic)
        for (int i=0; i<n_tot; i++) {
            correctForceWithCutoffTreeNeighborOneParticleImp<Tpsoft, Ttree, Tepj>(_sys[i], _tree, _acorr_flag);
        }
        auto& ap_manager = manager->ap_manager;
        const PS::S32 n_artificial = ap_manager.getArtificialParticleN();
#ifdef HARD_DEBUG
        assert((n_tot-_adr_ptcl_artificial_start)%n_artificial==0);
#endif
#pragma omp parallel for schedule(dynamic)
        for (int i=_adr_ptcl_artificial_start; i<n_tot; i+=n_artificial)
            ap_manager.correctArtficialParticleForce(&(_sys[i]));

    }

#ifdef HARD_DEBUG
public:
#endif
    //! Hard integration for clusters
    /* The local particle array are integrated. 
       No update of artificial particle pos and vel, eccept the artificial c.m. particle are kicked with acc. 
       The status of local particle in groups are set to 0 for first components if no tidal tensor memthod is used.
       @param[in,out] _ptcl_local: local particle in system_hard for integration
       @param[in] _n_ptcl: particle number in cluster
       @param[in,out] _ptcl_artificial: artificial particle array, c.m. are kicked 
       @param[in] _n_group: group number in cluster
       @param[in] _dt: integration ending time (initial time is fixed to 0)
       @param[in] _ithread: omp thread id, default 0
     */
    template <class Tsoft>
    void driveForMultiClusterImpl(PtclH4 * _ptcl_local,
                                  const PS::S32 _n_ptcl,
                                  Tsoft* _ptcl_artificial,
                                  const PS::S32 _n_group,
                                  const PS::F64 _dt,
                                  const PS::S32 _ithread=0) {
        ASSERT(checkParams());
#ifdef HARD_CHECK_ENERGY
        std::map<PS::S32, PS::S32> N_count;  // counting number of particles in one cluster
        PS::F64 ekin, epot, ekin_sd, epot_sd, de, de_sd, de_sd_change_cum;
#endif
#ifdef HARD_DEBUG_PROFILE
        N_count[_n_ptcl]++;
#endif
//#ifdef HARD_DEBUG_PRINT
//        PS::ReallocatableArray<PtclH4> ptcl_bk_pt;
//        ptcl_bk_pt.reserve(_n_ptcl);
//        ptcl_bk_pt.resizeNoInitialize(_n_ptcl);
//#endif

#ifdef HARD_DEBUG
        if (_n_ptcl>400) {
            std::cerr<<"Large cluster, n_ptcl="<<_n_ptcl<<" n_group="<<_n_group<<std::endl;
            for (PS::S32 i=0; i<_n_ptcl; i++) {
                if(_ptcl_local[i].r_search>10*_ptcl_local[i].r_search_min) {
                    std::cerr<<"i = "<<i<<" ";
                    _ptcl_local[i].print(std::cerr);
                    std::cerr<<std::endl;
                }
            }
        }
#endif
        const PS::F64 time_origin_int = 0.0; // to avoid precision issue
        const PS::F64 time_end = time_origin_int + _dt;

        // prepare initial groups with artificial particles
        PS::S32 adr_first_ptcl[_n_group+1];
        PS::S32 n_group_offset[_n_group+1]; // ptcl member offset in _ptcl_local
        n_group_offset[0] = 0;

        auto& ap_manager = manager->ap_manager;
        for(int i=0; i<_n_group; i++) {
            adr_first_ptcl[i] = i*ap_manager.getArtificialParticleN();
            auto* pi = &(_ptcl_artificial[adr_first_ptcl[i]]);
            n_group_offset[i+1] = n_group_offset[i] + ap_manager.getMemberN(pi);
            // pre-process for c.m. particle
            auto* pcm = ap_manager.getCMParticles(pi);
            // recover mass
            pcm->mass = pcm->mass_bk.d;

#ifdef ARTIFICIAL_PARTICLE_DEBUG
            ap_manager.checkConsistence(&_ptcl_local[n_group_offset[i]], &(_ptcl_artificial[adr_first_ptcl[i]]));
#endif
        }

#ifdef HARD_DEBUG
        if(_n_group>0) {
            if(n_group_offset[_n_group]<_n_ptcl)
                assert(_ptcl_local[n_group_offset[_n_group]].status.d==0);
            assert(_ptcl_local[n_group_offset[_n_group]-1].status.d<0);
        }
#endif

        // single particle start index in _ptcl_local
        PS::S32 i_single_start = n_group_offset[_n_group];
        // number of single particles
        PS::S32 n_single_init = _n_ptcl - i_single_start;
#ifdef HARD_DEBUG
        assert(n_single_init>=0);
#endif

#ifdef HARD_DEBUG
        // check member consistence
        for(int i=0; i<i_single_start; i++) {
            assert(_ptcl_local[i].status.d<0);
            assert(_ptcl_local[i].mass>0);
        }
#endif



#ifdef HARD_DEBUG_PRINT
        std::cerr<<"Hard: n_ptcl: "<<_n_ptcl<<" n_group: "<<_n_group<<std::endl;
#endif

        // manager
        H4::HermiteManager<HermiteInteraction>* h4_manager = &(manager->h4_manager);
        AR::SymplecticManager<ARInteraction>* ar_manager = &(manager->ar_manager);

        // Only one group with all particles in group
        if(_n_group==1&&n_single_init==0) {

            AR::SymplecticIntegrator<H4::ParticleAR<PtclHard>, PtclH4, ARPerturber, ARInteraction, H4::ARInformation<PtclHard>> sym_int;
            sym_int.manager = ar_manager;

            sym_int.particles.setMode(COMM::ListMode::copy);
            auto* api = &(_ptcl_artificial[adr_first_ptcl[0]]);
            const PS::S32 n_members = ap_manager.getMemberN(api);
            sym_int.particles.reserveMem(n_members);
            sym_int.info.reserveMem(n_members);
            //sym_int.perturber.r_crit_sq = h4_manager->r_neighbor_crit*h4_manager->r_neighbor_crit;
            for (PS::S32 i=0; i<n_members; i++) {
                sym_int.particles.addMemberAndAddress(_ptcl_local[i]);
                sym_int.info.particle_index.addMember(i);
                sym_int.info.r_break_crit = std::max(sym_int.info.r_break_crit,_ptcl_local[i].getRBreak());
                Float r_neighbor_crit = _ptcl_local[i].getRNeighbor();
                sym_int.perturber.r_neighbor_crit_sq = std::max(sym_int.perturber.r_neighbor_crit_sq, r_neighbor_crit*r_neighbor_crit);                
            }
            sym_int.reserveIntegratorMem();
            sym_int.info.generateBinaryTree(sym_int.particles);
            auto* apcm = ap_manager.getCMParticles(api);
            auto* aptt = ap_manager.getTidalTensorParticles(api);
            TidalTensor tt;
            tt.fit(aptt, *apcm, ap_manager.r_tidal_tensor);
            sym_int.perturber.soft_pert=&tt;

            // calculate soft_pert_min
            sym_int.perturber.calcSoftPertMin(sym_int.info.getBinaryTreeRoot());
            
            // initialization 
            sym_int.initialIntegration(time_origin_int);
            sym_int.info.calcDsAndStepOption(sym_int.slowdown.getSlowDownFactorOrigin(), ar_manager->step.getOrder()); 

            // calculate c.m. changeover
            auto& pcm = sym_int.particles.cm;
            PS::F64 m_fac = pcm.mass*Ptcl::mean_mass_inv;
            pcm.changeover.setR(m_fac, manager->ap_manager.r_in_base, manager->ap_manager.r_out_base);

            // set tt gid
            sym_int.perturber.soft_pert->group_id = pcm.changeover.getRout();

            //check paramters
            ASSERT(sym_int.info.checkParams());
            ASSERT(sym_int.perturber.checkParams());

            // integration
            sym_int.integrateToTime(time_end);

            pcm.pos += pcm.vel * _dt;

            // update rsearch
            pcm.Ptcl::calcRSearch(_dt);
            // copyback
            sym_int.particles.shiftToOriginFrame();
            sym_int.particles.template writeBackMemberAll<PtclH4>();

            for (PS::S32 i=0; i<n_members; i++) {
                auto& pi = _ptcl_local[i];
                pi.r_search = std::max(pcm.r_search, pi.r_search);
#ifdef CLUSTER_VELOCITY
                pi.status.f[0]  = pcm.vel[0];
                pi.status.f[1]  = pcm.vel[1];
                pi.mass_bk.f[0] = pcm.vel[2];
                pi.mass_bk.f[1] = pcm.mass;
#endif
#ifdef HARD_DEBUG
                ASSERT(_ptcl_local[i].r_search>_ptcl_local[i].changeover.getRout());
#endif
            }

#ifdef PROFILE
            ARC_substep_sum += sym_int.profile.step_count;
            ARC_n_groups += 1;
#endif
#ifdef HARD_CHECK_ENERGY
            PS::F64 kappa_inv = 1.0/sym_int.slowdown.getSlowDownFactor();
            ekin    = sym_int.getEkin();
            epot    = sym_int.getEpot();
            de      = sym_int.getEnergyError();
#ifdef AR_SLOWDOWN_INNER
            ekin_sd = kappa_inv*sym_int.getEkinSlowDownInner();
            epot_sd = kappa_inv*sym_int.getEpotSlowDownInner();
            de_sd   = kappa_inv*sym_int.getEnergyErrorSlowDownInner();
#else
            ekin_sd = ekin;
            epot_sd = epot;
            de_sd   = de;
#endif
            de_sd_change_cum = sym_int.getDESlowDownChangeCum();
#endif
        }
        else {
            // integration -----------------------------
            H4::HermiteIntegrator<PtclHard, PtclH4, HermitePerturber, ARPerturber, HermiteInteraction, ARInteraction, HermiteInformation> h4_int;
            h4_int.manager = h4_manager;
            h4_int.ar_manager = ar_manager;

            h4_int.particles.setMode(COMM::ListMode::link);
            h4_int.particles.linkMemberArray(_ptcl_local, _n_ptcl);

            h4_int.particles.calcCenterOfMass();
            h4_int.particles.shiftToCenterOfMassFrame();
            
            PS::S32 n_group_size_max = _n_group+_n_group/2+5;
            h4_int.groups.setMode(COMM::ListMode::local);
            h4_int.groups.reserveMem(n_group_size_max);
            h4_int.reserveIntegratorMem();

            // initial system 
            h4_int.initialSystemSingle(0.0);

            // Tidal tensor 
            TidalTensor tidal_tensor[n_group_size_max];
            PS::S32 n_tt = 0;
            
            // add groups
            if (_n_group>0) {
                ASSERT(n_group_offset[_n_group]>0);
                PS::S32 ptcl_index_group[n_group_offset[_n_group]];
                for (PS::S32 i=0; i<n_group_offset[_n_group]; i++) ptcl_index_group[i] = i;
                h4_int.addGroups(ptcl_index_group, n_group_offset, _n_group);

                for (PS::S32 i=0; i<_n_group; i++) {
                    auto* api = &(_ptcl_artificial[adr_first_ptcl[i]]);
                    auto* aptt = ap_manager.getTidalTensorParticles(api);
                    auto* apcm = ap_manager.getCMParticles(api);
                    // correct pos for t.t. cm
                    apcm->pos -= h4_int.particles.cm.pos;
                    tidal_tensor[i].fit(aptt, *apcm, ap_manager.r_tidal_tensor);
                    n_tt ++;
                    auto& groupi = h4_int.groups[i];
                    groupi.perturber.soft_pert = &tidal_tensor[i];

                    // calculate soft_pert_min
                    groupi.perturber.calcSoftPertMin(groupi.info.getBinaryTreeRoot());

                    // calculate c.m. changeover
                    auto& pcm = groupi.particles.cm;
                    PS::F64 m_fac = pcm.mass*Ptcl::mean_mass_inv;

                    ASSERT(m_fac>0.0);
                    pcm.changeover.setR(m_fac, manager->ap_manager.r_in_base, manager->ap_manager.r_out_base);

#ifdef HARD_DEBUG
                    PS::F64 r_out_cm = pcm.changeover.getRout();
                    for (PS::S32 k=0; k<groupi.particles.getSize(); k++) 
                        ASSERT(abs(groupi.particles[k].changeover.getRout()-r_out_cm)<1e-10);
#endif
                    // set group id of tidal tensor by r out.
                    groupi.perturber.soft_pert->group_id = pcm.changeover.getRout();
                }
            }

            // initialization 
            h4_int.initialIntegration(); // get neighbors and min particles
            // AR inner slowdown number
            int n_group_sub_init[_n_group], n_group_sub_tot_init=0;
#ifdef AR_SLOWDOWN_INNER
            for (int i=0; i<_n_group; i++) {
                n_group_sub_init[i] = h4_int.groups[i].slowdown_inner.getSize();
                n_group_sub_tot_init += n_group_sub_init[i];
            }
#else
            for (int i=0; i<_n_group; i++) n_group_sub_init[i] = 0;
#endif
            h4_int.adjustGroups(true);

            const PS::S32 n_init = h4_int.getNInitGroup();
            const PS::S32* group_index = h4_int.getSortDtIndexGroup();
            for(int i=0; i<n_init; i++) {
                auto& groupi = h4_int.groups[group_index[i]];
                // calculate c.m. changeover
                auto& pcm = groupi.particles.cm;
                PS::F64 m_fac = pcm.mass*Ptcl::mean_mass_inv;
                ASSERT(m_fac>0.0);
                pcm.changeover.setR(m_fac, manager->ap_manager.r_in_base, manager->ap_manager.r_out_base);

#ifdef SOFT_PERT                
                // check whether all r_out are same (primoridal or not)
                bool primordial_flag = true;
                PS::F64 r_out_cm = groupi.particles.cm.changeover.getRout();
                for (PS::S32 k=0; k<groupi.particles.getSize(); k++) 
                    if (abs(groupi.particles[k].changeover.getRout()-r_out_cm)>1e-10) {
                        primordial_flag =false;
                        break;
                    }
                if (n_tt>0 && primordial_flag) {
                    // check closed tt and only find consistent changeover 
                    PS::F32 tt_index=groupi.perturber.findCloseSoftPert(tidal_tensor, n_tt, n_group_size_max, groupi.particles.cm, r_out_cm);
                    ASSERT(tt_index<n_tt);
                    // calculate soft_pert_min
                    if (tt_index>=0) 
                        groupi.perturber.calcSoftPertMin(groupi.info.getBinaryTreeRoot());
#ifdef HARD_DEBUG_PRINT
                    std::cerr<<"Find tidal tensor, group i: "<<group_index[i]<<" pcm.r_out: "<<r_out_cm;
                    std::cerr<<" member.r_out: ";
                    for (PS::S32 k=0; k<groupi.particles.getSize(); k++) 
                        std::cerr<<groupi.particles[k].changeover.getRout()<<" ";
                    std::cerr<<" tidal tensor index: "<<tt_index;
                    std::cerr<<std::endl;
#endif
                    tt_index=0;
                }
#endif
            }

            h4_int.initialIntegration();
            h4_int.sortDtAndSelectActParticle();
            h4_int.info.time_origin = h4_int.getTime() + time_origin_;

#ifdef HARD_CHECK_ENERGY
            h4_int.calcEnergySlowDown(true);
#endif

#ifdef HARD_DEBUG_PRINT_TITLE
            h4_int.printColumnTitle(std::cout, WRITE_WIDTH, n_group_sub_init, _n_group, n_group_sub_tot_init);
            std::cout<<std::endl;
#endif
#ifdef HARD_DEBUG_PRINT
            h4_int.printColumn(std::cout, WRITE_WIDTH, n_group_sub_init, _n_group, n_group_sub_tot_init);
            std::cout<<std::endl;
#endif
            // integration loop
            while (h4_int.getTime()<_dt) {

                h4_int.integrateOneStepAct();
                h4_int.adjustGroups(false);

                const PS::S32 n_init_group = h4_int.getNInitGroup();
                const PS::S32 n_act_group = h4_int.getNActGroup();
                const PS::S32* group_index = h4_int.getSortDtIndexGroup();
                for(int i=0; i<n_init_group; i++) {
                    auto& groupi = h4_int.groups[group_index[i]];
                    // calculate c.m. changeover
                    auto& pcm = groupi.particles.cm;
                    PS::F64 m_fac = pcm.mass*Ptcl::mean_mass_inv;
                    ASSERT(m_fac>0.0);
                    pcm.changeover.setR(m_fac, manager->ap_manager.r_in_base, manager->ap_manager.r_out_base);

#ifdef SOFT_PERT                
                    // check whether all r_out are same (primoridal or not)
                    bool primordial_flag = true;
                    PS::F64 r_out_cm = groupi.particles.cm.changeover.getRout();
                    for (PS::S32 k=0; k<groupi.particles.getSize(); k++) 
                        if (abs(groupi.particles[k].changeover.getRout()-r_out_cm)>1e-10) {
                            primordial_flag =false;
                            break;
                        }

                    if (n_tt>0 && primordial_flag) {
                        // check closed tt and only find consistent changeover 
                        PS::F32 tt_index=groupi.perturber.findCloseSoftPert(tidal_tensor, n_tt, n_group_size_max, groupi.particles.cm, r_out_cm);
                        ASSERT(tt_index<n_tt);
                        // calculate soft_pert_min
                        if (tt_index>=0) 
                            groupi.perturber.calcSoftPertMin(groupi.info.getBinaryTreeRoot());
#ifdef HARD_DEBUG_PRINT
                        std::cerr<<"Find tidal tensor, group i: "<<group_index[i]<<" pcm.r_out: "<<groupi.particles.cm.changeover.getRout();
                        std::cerr<<" member.r_out: ";
                        for (PS::S32 k=0; k<groupi.particles.getSize(); k++) 
                            std::cerr<<groupi.particles[k].changeover.getRout()<<" ";
                        std::cerr<<" tidal tensor index: "<<tt_index;
                        std::cerr<<std::endl;
#endif

                    }
#endif
                }
                ASSERT(n_init_group<=n_act_group);
#ifdef SOFT_PERT
                // update c.m. for Tidal tensor
                if (n_tt>0) {
                    for(int i=n_init_group; i<n_act_group; i++) {
                        auto& groupi = h4_int.groups[group_index[i]];
                        if (groupi.perturber.soft_pert!=NULL) 
                            groupi.perturber.soft_pert->shiftCM(groupi.particles.cm.pos);
                    }
                }
#endif
                // initial after groups are modified
                h4_int.initialIntegration();
                h4_int.sortDtAndSelectActParticle();
                h4_int.info.time_origin = h4_int.getTime() + time_origin_;

#ifdef HARD_DEBUG_PRINT
                //PS::F64 dt_max = 0.0;
                //PS::S32 n_group = h4_int.getNGroup();
                //PS::S32 n_single = h4_int.getNSingle();
                //if (n_group>0) dt_max = h4_int.groups[h4_int.getSortDtIndexGroup()[n_group-1]].particles.cm.dt;
                //if (n_single>0) dt_max = std::max(dt_max, h4_int.particles[h4_int.getSortDtIndexSingle()[n_single-1]].dt);
                //ASSERT(dt_max>0.0);
                if (fmod(h4_int.getTime(), h4_manager->step.getDtMax()/HARD_DEBUG_PRINT_FEQ)==0.0) {
                    h4_int.calcEnergySlowDown(false);

                    h4_int.printColumn(std::cout, WRITE_WIDTH, n_group_sub_init, _n_group, n_group_sub_tot_init);
                    std::cout<<std::endl;

                    de_sd   = h4_int.getEnergyErrorSlowDown();
                    if (abs(de_sd) > manager->energy_error_max) {
                        ekin    = h4_int.getEkin();
                        ekin_sd = h4_int.getEkinSlowDown();
                        epot    = h4_int.getEpot();
                        epot_sd = h4_int.getEpotSlowDown();
                        PS::F64 etot_sd = h4_int.getEtotSlowDownRef();
                        de      = h4_int.getEnergyError();
                        de_sd_change_cum = h4_int.getDESlowDownChangeCum();
                        std::cerr<<"Hard energy significant ("<<de_sd<<") !"
                                 <<"  Ekin: "<<ekin
                                 <<"  Epot: "<<epot
                                 <<"  Ekin_SD: "<<ekin_sd
                                 <<"  Epot_SD: "<<epot_sd
                                 <<"  Etot_SD_ref: "<<etot_sd
                                 <<"  dE: "<<de
                                 <<"  dE_SD: "<<de_sd
                                 <<"  dE_SD_CHANGE: "<<de_sd_change_cum
                                 <<std::endl;
                        DATADUMP("hard_dump");
                        abort();
                    }
                }
                if (fmod(h4_int.getTime(), h4_manager->step.getDtMax())==0.0) {
                    h4_int.printStepHist();
                }
#endif
            }

#ifdef HARD_CHECK_ENERGY
            h4_int.calcEnergySlowDown(false);
            ekin    = h4_int.getEkin();
            ekin_sd = h4_int.getEkinSlowDown();
            epot    = h4_int.getEpot();
            epot_sd = h4_int.getEpotSlowDown();
            de      = h4_int.getEnergyError();
            de_sd   = h4_int.getEnergyErrorSlowDown();
            de_sd_change_cum = h4_int.getDESlowDownChangeCum();
#else
            h4_int.writeBackGroupMembers();
#endif
            h4_int.particles.cm.pos += h4_int.particles.cm.vel * _dt;

            h4_int.particles.shiftToOriginFrame();

            // update research and status
            auto& h4_pcm = h4_int.particles.cm;
            for(PS::S32 i=0; i<h4_int.getNGroup(); i++) {
                const PS::S32 k =group_index[i];
#ifdef HARD_DEBUG
                ASSERT(h4_int.groups[k].particles.cm.changeover.getRout()>0);
#endif
                //h4_int.groups[k].particles.cm.calcRSearch(_dt);
                auto& pcm = h4_int.groups[k].particles.cm;
                pcm.vel += h4_pcm.vel;
//#ifdef HARD_DEBUG
//                ASSERT(h4_pcm.mass-pcm.mass>=0);
//#endif
                //pcm.calcRSearch(h4_manager->interaction.G*(h4_pcm.mass-pcm.mass), abs(pcm.pot), h4_pcm.vel, _dt);
                pcm.Ptcl::calcRSearch(_dt);
                const PS::S32 n_member = h4_int.groups[k].particles.getSize();
                //const PS::S32 id_first = h4_int.groups[k].particles.getMemberOriginAddress(0)->id;
                for (PS::S32 j=0; j<n_member; j++) {
                    auto* pj = h4_int.groups[k].particles.getMemberOriginAddress(j);
                    pj->r_search = std::max(pj->r_search, pcm.r_search);
                    // set new c.m. id as status for searchneighbors
                    //pj->status = -id_first;
                    // save c.m. velocity and mass for neighbor search
#ifdef CLUSTER_VELOCITY
                    pj->status.f[0] = pcm.vel[0];
                    pj->status.f[1] = pcm.vel[1];
                    pj->mass_bk.f[0]= pcm.vel[2];
                    pj->mass_bk.f[1]= pcm.mass;
#endif
#ifdef HARD_DEBUG
                    ASSERT(pj->r_search>pj->changeover.getRout());
#endif
                }
            }
            const PS::S32* single_index = h4_int.getSortDtIndexSingle();
            for (PS::S32 i=0; i<h4_int.getNSingle(); i++) {
                auto& pi = h4_int.particles[single_index[i]];
                // set status for searchneighbors
#ifdef CLUSTER_VELOCITY
                pi.status.f[0]  = 0.0;
                pi.status.f[1]  = 0.0;
                pi.mass_bk.f[0] = 0.0;
                pi.mass_bk.f[1] = 0.0;
#endif
//#ifdef HARD_DEBUG
//                ASSERT(h4_pcm.mass-pi.mass>0);
//#endif
                pi.Ptcl::calcRSearch(_dt);
//                pi.calcRSearch(h4_manager->interaction.G*(h4_pcm.mass-pi.mass), abs(pi.pot), h4_pcm.vel, _dt);
            }


#ifdef PROFILE
            //ARC_substep_sum += Aint.getNsubstep();
            H4_step_sum += h4_int.profile.hermite_single_step_count + h4_int.profile.hermite_group_step_count;
            ARC_substep_sum += h4_int.profile.ar_step_count;
            ARC_tsyn_step_sum += h4_int.profile.ar_step_count_tsyn;
            ARC_n_groups += _n_group;
            if (h4_int.profile.ar_step_count>manager->ar_manager.step_count_max) {
                std::cerr<<"Large AR step cluster found: step: "<<h4_int.profile.ar_step_count<<std::endl;
                DATADUMP("dump_large_step");
            } 
#endif
#ifdef AR_DEBUG_PRINT
            for (PS::S32 i=0; i<h4_int.getNGroup(); i++) {
                const PS::S32 k= group_index[i];
                auto& groupk = h4_int.groups[k];
                std::cerr<<"Group N:"<<std::setw(6)<<ARC_n_groups
                         <<" k:"<<std::setw(2)<<k
                         <<" N_member: "<<std::setw(4)<<groupk.particles.getSize()
                         <<" step: "<<std::setw(12)<<groupk.profile.step_count_sum
                         <<" step(tsyn): "<<std::setw(10)<<groupk.profile.step_count_tsyn_sum
//                         <<" step(sum): "<<std::setw(12)<<h4_int.profile.ar_step_count
//                         <<" step_tsyn(sum): "<<std::setw(12)<<h4_int.profile.ar_step_count_tsyn
                         <<" Soft_Pert: "<<std::setw(20)<<groupk.perturber.soft_pert_min
                         <<" Pert_In: "<<std::setw(20)<<groupk.slowdown.getPertIn()
                         <<" Pert_Out: "<<std::setw(20)<<groupk.slowdown.getPertOut()
                         <<" SD: "<<std::setw(20)<<groupk.slowdown.getSlowDownFactor()
                         <<" SD(org): "<<std::setw(20)<<groupk.slowdown.getSlowDownFactorOrigin();
                auto& bin = groupk.info.getBinaryTreeRoot();
                std::cerr<<" semi: "<<std::setw(20)<<bin.semi
                         <<" ecc: "<<std::setw(20)<<bin.ecc
                         <<" period: "<<std::setw(20)<<bin.period
                         <<" NB: "<<std::setw(4)<<groupk.perturber.neighbor_address.getSize()
                         <<std::endl;
                if (groupk.profile.step_count_tsyn_sum>10000) {
                    std::string dumpname="hard_dump."+std::to_string(int(ARC_n_groups));
                    DATADUMP(dumpname.c_str());
                }
            }
#endif
        }

#ifdef HARD_CHECK_ENERGY
        energy.de_sd += de_sd;
        energy.de    += de;
        PS::F64 ekin_sd_correction = ekin_sd - ekin;
        PS::F64 epot_sd_correction = epot_sd - epot;
        energy.ekin_sd_correction += ekin_sd_correction;
        energy.epot_sd_correction += epot_sd_correction;
        energy.de_sd_change_cum   += de_sd_change_cum - (ekin_sd_correction + epot_sd_correction);
#ifdef HARD_DEBUG_PRINT
        std::cerr<<"Hard Energy: "
                 <<"  Ekin: "<<ekin
                 <<"  Ekin_SD: "<<ekin_sd
                 <<"  Epot: "<<epot_sd
                 <<"  Epot_SD: "<<epot_sd
                 <<"  dE: "<<de
                 <<"  dE_SD: "<<de_sd
                 <<"  dE_SD_CHANGE: "<<de_sd_change_cum
                 <<std::endl;
#endif        
#ifdef HARD_CLUSTER_PRINT
        std::cerr<<"Hard cluster: dE_SD: "<<de_sd
                 <<" dE: "<<de
                 <<" Ekin: "<<ekin
                 <<" Epot: "<<epot
                 <<" Ekin_SD: "<<ekin_sd
                 <<" Epot_SD: "<<epot_sd
                 <<" H4_step(single): "<<H4_step_sum
                 <<" AR_step: "<<ARC_substep_sum
                 <<" AR_step(tsyn): "<<ARC_tsyn_step_sum
                 <<" n_ptcl: "<<_n_ptcl
                 <<" n_group: "<<_n_group
                 <<std::endl;
#endif
        if (abs(de_sd) > manager->energy_error_max) {
            std::cerr<<"Hard energy significant ("<<de_sd<<") !\n";
            DATADUMP("hard_large_energy");
            //abort();
        }
#endif
    }

public:

    SystemHard(){
        manager = NULL;
#ifdef HARD_DEBUG_PROFILE
        for(PS::S32 i=0;i<20;i++) N_count[i]=0;
#endif
#ifdef PROFILE
        ARC_substep_sum = 0;
        ARC_tsyn_step_sum =0;
        ARC_n_groups = 0;
        H4_step_sum = 0;
#endif
#ifdef HARD_CHECK_ENERGY
        energy.clear();
#endif
        //        PS::S32 n_threads = PS::Comm::getNumberOfThread();
    }

    void initializeForOneCluster(const PS::S32 n){
#ifdef HARD_DEBUG
        assert(n<ARRAY_ALLOW_LIMIT);
#endif        
        ptcl_hard_.resizeNoInitialize(n);
    }

    ////////////////////////
    // for NON-ISOLATED CLUSTER
    template<class Tsys, class Tptcl, class Tmediator>
    void setPtclForConnectedCluster(const Tsys & sys,
                                   const PS::ReallocatableArray<Tmediator> & med,
                                   const PS::ReallocatableArray<Tptcl> & ptcl_recv){
        ptcl_hard_.clearSize();
        n_ptcl_in_cluster_.clearSize(); // clear befor break this function
        for(PS::S32 i=0; i<med.size(); i++){
            if(med[i].adr_sys_ < 0) continue;
            if(med[i].rank_send_ != PS::Comm::getRank()) continue;
            const auto & p = sys[med[i].adr_sys_];
            ptcl_hard_.push_back(PtclHard(p, med[i].id_cluster_, med[i].adr_sys_));
#ifdef HARD_DEBUG
            assert(med[i].adr_sys_<sys.getNumberOfParticleLocal());
            if(p.id<0&&p.status.d<0) {
                std::cerr<<"Error: ghost particle is selected! i="<<i<<"; med[i].adr_sys="<<med[i].adr_sys_<<std::endl;
                abort();
            }
#endif
        }

        for(PS::S32 i=0; i<ptcl_recv.size(); i++){
            const Tptcl & p = ptcl_recv[i];
            ptcl_hard_.push_back(PtclHard(p, p.id_cluster, -(i+1)));
#ifdef HARD_DEBUG
            if(p.id<0&&p.status.d<0) {
                std::cerr<<"Error: receive ghost particle! i="<<i<<std::endl;
                abort();
            }
#endif
        }

        if(ptcl_hard_.size() == 0) return;
        std::sort(ptcl_hard_.getPointer(), ptcl_hard_.getPointer(ptcl_hard_.size()), 
                  OPLessIDCluster());
        PS::S32 n_tot = ptcl_hard_.size();
        PS::S32 id_cluster_ref = -999;
        for(PS::S32 i=0; i<n_tot; i++){
            if(id_cluster_ref != ptcl_hard_[i].id_cluster){
                id_cluster_ref = ptcl_hard_[i].id_cluster;
                n_ptcl_in_cluster_.push_back(0);
            }
            n_ptcl_in_cluster_.back()++;
        }
        PS::S32 n_cluster = n_ptcl_in_cluster_.size();
#ifdef HARD_DEBUG
        assert(n_cluster<ARRAY_ALLOW_LIMIT);
#endif        
        n_ptcl_in_cluster_disp_.resizeNoInitialize(n_cluster+1);
        n_ptcl_in_cluster_disp_[0] = 0;
        for(PS::S32 i=0; i<n_cluster; i++){
#ifdef HARD_DEBUG
            assert(n_ptcl_in_cluster_[i]>1);
#endif
            n_ptcl_in_cluster_disp_[i+1] = n_ptcl_in_cluster_disp_[i] + n_ptcl_in_cluster_[i];
        }
    }


    // for NON-ISOLATED CLUSTER
    ////////////////////////

    PS::S32 getGroupPtclRemoteN() const{
        return n_group_member_remote_;
    }

    PS::ReallocatableArray<PtclH4> & getPtcl() {
        return ptcl_hard_;
    }

    PS::S32 getNCluster() const{
        return n_ptcl_in_cluster_.size();
    }

    PS::S32* getClusterNList(const std::size_t i=0) const{
        return n_ptcl_in_cluster_.getPointer(i);
    }

    PS::S32* getClusterNOffset(const std::size_t i=0) const{
        return n_ptcl_in_cluster_disp_.getPointer(i);
    }

    PS::S32* getGroupNList(const std::size_t i=0) const{
        return n_group_in_cluster_.getPointer(i);
    }

    PS::S32* getGroupNOffset(const std::size_t i=0) const{
        return n_group_in_cluster_offset_.getPointer(i);
    }

    PS::S32* getAdrPtclArtFirstList(const std::size_t i=0) const{
        return adr_first_ptcl_arti_in_cluster_.getPointer(i);
    }

    PS::S32 getNClusterChangeOverUpdate() const {
        return i_cluster_changeover_update_.size();
    }

    void setTimeOrigin(const PS::F64 _time_origin){
        time_origin_ = _time_origin;
    }

    //void setParam(const PS::F64 _rbin,
    //              const PS::F64 _rout,
    //              const PS::F64 _rin,
    //              const PS::F64 _eps,
    //              const PS::F64 _dt_limit_hard,
    //              const PS::F64 _dt_min_hard,
    //              const PS::F64 _eta,
    //              const PS::F64 _time_origin,
    //              const PS::F64 _sd_factor,
    //              const PS::F64 _v_max,
    //              // const PS::F64 _gmin,
    //              // const PS::F64 _m_avarage,
    //              const PS::S64 _id_offset,
    //              const PS::S32 _n_split){
    //    /// Set chain pars (L.Wang)
	//    Int_pars_.rin  = _rin;
    //    Int_pars_.rout = _rout;
    //    Int_pars_.r_oi_inv = 1.0/(_rout-_rin);
    //    Int_pars_.r_A      = (_rout-_rin)/(_rout+_rin);
    //    Int_pars_.pot_off  = (1.0+Int_pars_.r_A)/_rout;
    //    Int_pars_.eps2  = _eps*_eps;
    //    Int_pars_.r_bin = _rbin;
    //    /// Set chain pars (L.Wang)        
    //    dt_limit_hard_ = _dt_limit_hard;
    //    dt_min_hard_   = _dt_min_hard;
    //    eta_s_ = _eta*_eta;
    //    sdfactor_ = _sd_factor;
    //    v_max_ = _v_max;
    //    time_origin_ = _time_origin;
//  //      gamma_ = std::pow(1.0/_gmin,0.33333);
    //    // r_search_single_ = _rsearch; 
    //    //r_bin_           = _rbin;
    //    // m_average_ = _m_avarage;
    //    manager->n_split = _n_split;
    //    id_offset_ = _id_offset;
    //}

//    void updateRSearch(PtclH4* ptcl_org,
//                       const PS::S32* ptcl_list,
//                       const PS::S32 n_ptcl,
//                       const PS::F64 dt_tree) {
//        for (PS::S32 i=0; i<n_ptcl; i++) {
//            ptcl_org[ptcl_list[i]].calcRSearch(dt_tree);
//        }
//    }

//////////////////
// for one cluster
    template<class Tsys>
    void setPtclForOneCluster(const Tsys & sys, 
                              const PS::ReallocatableArray<PS::S32> & adr_array){
        // for one cluster
        const PS::S32 n = adr_array.size();
        //ptcl_hard_.resizeNoInitialize(n);
        //n_ptcl_in_cluster_.resizeNoInitialize(n);
        for(PS::S32 i=0; i<n; i++){
            PS::S32 adr = adr_array[i];
            ptcl_hard_[i].DataCopy(sys[adr]);
            ptcl_hard_[i].adr_org = adr;
            //n_ptcl_in_cluster_[i] = 1;
        }
    }

    template<class Tsys>
    void setPtclForOneClusterOMP(const Tsys & sys, 
                                 const PS::ReallocatableArray<PS::S32> & adr_array){
        // for one cluster
        const PS::S32 n = adr_array.size();
        //ptcl_hard_.resizeNoInitialize(n);
        //n_ptcl_in_cluster_.resizeNoInitialize(n);
#pragma omp parallel for schedule(dynamic)
        for(PS::S32 i=0; i<n; i++){
            PS::S32 adr = adr_array[i];
            ptcl_hard_[i].DataCopy(sys[adr]);
            ptcl_hard_[i].adr_org = adr;
            //n_ptcl_in_cluster_[i] = 1;
        }
    }


    //! integrate one isolated particle
    /*! integrate one isolated particle and calculate new r_search
      @param[in] _dt: tree time step
     */
    void driveForOneCluster(const PS::F64 _dt) {
        const PS::S32 n = ptcl_hard_.size();
        for(PS::S32 i=0; i<n; i++){
            PS::F64vec dr = ptcl_hard_[i].vel * _dt;
            ptcl_hard_[i].pos += dr;
            ptcl_hard_[i].Ptcl::calcRSearch(_dt);
#ifdef HARD_DEBUG
            // to avoid issue in cluster search with velocity
            assert(ptcl_hard_[i].status.d==0.0);
            assert(ptcl_hard_[i].mass_bk.d==0.0);
#endif
            // ptcl_hard_[i].r_search= r_search_single_;
            /*
              DriveKeplerRestricted(mass_sun_, 
              pos_sun_, ptcl_hard_[i].pos, 
              vel_sun_, ptcl_hard_[i].vel, dt); 
            */
        }

    }

    //! integrate one isolated particle
    /*! integrate one isolated particle and calculate new r_search
      @param[in] _dt: tree time step
      @param[in] _v_max: maximum velocity used to calculate r_search
     */
    void driveForOneClusterOMP(const PS::F64 _dt) {
        const PS::S32 n = ptcl_hard_.size();
#pragma omp parallel for schedule(dynamic)
        for(PS::S32 i=0; i<n; i++){
            PS::F64vec dr = ptcl_hard_[i].vel * _dt;
            ptcl_hard_[i].pos += dr;
#ifdef HARD_DEBUG
            // to avoid issue in cluster search with velocity
            assert(ptcl_hard_[i].status.d==0.0);
            assert(ptcl_hard_[i].mass_bk.d==0.0);
#endif
            ptcl_hard_[i].Ptcl::calcRSearch(_dt);
            /*
              DriveKeplerRestricted(mass_sun_, 
              pos_sun_, ptcl_hard_[i].pos, 
              vel_sun_, ptcl_hard_[i].vel, dt); 
            */
        }
    }

    template<class Tsys>
    void writeBackPtclForOneCluster(Tsys & sys, 
//                                    const PS::ReallocatableArray<PS::S32> & adr_array,
                                    PS::ReallocatableArray<PS::S32> & removelist){
        const PS::S32 n = ptcl_hard_.size();
        //PS::ReallocatableArray<PS::S32> removelist(n);
        for(PS::S32 i=0; i<n; i++){
            //PS::S32 adr = adr_array[i];
            PS::S32 adr = ptcl_hard_[i].adr_org;
#ifdef HARD_DEBUG
            assert(sys[adr].id == ptcl_hard_[i].id);
#endif
            sys[adr].DataCopy(ptcl_hard_[i]);
            if(sys[adr].id<0&&sys[adr].status.d<0) removelist.push_back(adr);
        }
        //sys.removeParticle(removelist.getPointer(), removelist.size());
    }

    template<class Tsys>
    void writeBackPtclForOneClusterOMP(Tsys & sys){
        const PS::S32 n = ptcl_hard_.size();
#pragma omp parallel for schedule(dynamic)
        for(PS::S32 i=0; i<n; i++){
            PS::S32 adr = ptcl_hard_[i].adr_org;
            //PS::S32 adr = adr_array[i];
#ifdef HARD_DEBUG
            assert(sys[adr].id == ptcl_hard_[i].id);
#endif
            sys[adr].DataCopy(ptcl_hard_[i]);
        }
    }

    template<class Tsys>
    void writeBackPtclLocalOnlyOMP(Tsys & sys) {
        const PS::S32 n = ptcl_hard_.size();
#pragma omp parallel for schedule(dynamic)
        for(PS::S32 i=0; i<n; i++){
            PS::S32 adr = ptcl_hard_[i].adr_org;
            //PS::S32 adr = adr_array[i];
#ifdef HARD_DEBUG
            if(adr>=0) assert(sys[adr].id == ptcl_hard_[i].id);
#endif
            if(adr>=0) sys[adr].DataCopy(ptcl_hard_[i]);
        }
    }
// for one cluster
//////////////////


//////////////////
// for isolated multi cluster only
    template<class Tsys>
    void setPtclForIsolatedMultiCluster(const Tsys & sys,
                                        const PS::ReallocatableArray<PS::S32> & _adr_array,
                                        const PS::ReallocatableArray<PS::S32> & _n_ptcl_in_cluster){
        const PS::S32 n_cluster = _n_ptcl_in_cluster.size();
#ifdef HARD_DEBUG
        assert(n_cluster<ARRAY_ALLOW_LIMIT);
#endif        
        n_ptcl_in_cluster_.resizeNoInitialize(n_cluster);
        n_ptcl_in_cluster_disp_.resizeNoInitialize(n_cluster+1);
        n_ptcl_in_cluster_disp_[0] = 0;
        for(PS::S32 i=0; i<n_cluster; i++){
            n_ptcl_in_cluster_[i] = _n_ptcl_in_cluster[i];
#ifdef HARD_DEBUG
            assert(n_ptcl_in_cluster_[i]>1);
#endif
            n_ptcl_in_cluster_disp_[i+1] = n_ptcl_in_cluster_disp_[i] + n_ptcl_in_cluster_[i];
        }
        const PS::S32 n_ptcl = _adr_array.size();
#ifdef HARD_DEBUG
        assert(n_ptcl<ARRAY_ALLOW_LIMIT);
#endif        
        ptcl_hard_.resizeNoInitialize(n_ptcl);
        for(PS::S32 i=0; i<n_ptcl; i++){
            PS::S32 adr = _adr_array[i];
            ptcl_hard_[i].DataCopy(sys[adr]);
            ptcl_hard_[i].adr_org = adr;
            //  ptcl_hard_[i].n_ngb= sys[adr].n_ngb;
        }
    }

    void initailizeForIsolatedMultiCluster(const PS::S32 _n_ptcl,
                                           const PS::ReallocatableArray<PS::S32> & _n_ptcl_in_cluster){
#ifdef HARD_DEBUG
        assert(_n_ptcl<ARRAY_ALLOW_LIMIT);
#endif        
        ptcl_hard_.resizeNoInitialize(_n_ptcl);
        const PS::S32 n_cluster = _n_ptcl_in_cluster.size();
#ifdef HARD_DEBUG
        assert(n_cluster<ARRAY_ALLOW_LIMIT);
#endif        
        n_ptcl_in_cluster_.resizeNoInitialize(n_cluster);
        n_ptcl_in_cluster_disp_.resizeNoInitialize(n_cluster+1);
        n_ptcl_in_cluster_disp_[0] = 0;
        for(PS::S32 i=0; i<n_cluster; i++){
            n_ptcl_in_cluster_[i] = _n_ptcl_in_cluster[i];
#ifdef HARD_DEBUG
            assert(n_ptcl_in_cluster_[i]>1);
#endif
            n_ptcl_in_cluster_disp_[i+1] = n_ptcl_in_cluster_disp_[i] + n_ptcl_in_cluster_[i];
        }
    }

    template<class Tsys>
    void setPtclForIsolatedMultiClusterOMP(const Tsys & sys,
                                           const PS::ReallocatableArray<PS::S32> & _adr_array,
                                           const PS::ReallocatableArray<PS::S32> & _n_ptcl_in_cluster){
        const PS::S32 n_ptcl = _adr_array.size();
#pragma omp parallel for schedule(dynamic)
        for(PS::S32 i=0; i<n_ptcl; i++){
            PS::S32 adr = _adr_array[i];
            ptcl_hard_[i].DataCopy(sys[adr]);
            ptcl_hard_[i].adr_org = adr;
            //  ptcl_hard_[i].n_ngb = sys[adr].n_ngb;
        }
    }

    template<class Tsys>
    void writeBackPtclForMultiCluster(Tsys & sys, 
//                                      const PS::ReallocatableArray<PS::S32> & adr_array,
                                      PS::ReallocatableArray<PS::S32> & removelist){
        writeBackPtclForOneCluster(sys, removelist);
    }

    template<class Tsys>
    void writeBackPtclForMultiClusterOMP(Tsys & sys) { 
        writeBackPtclForOneClusterOMP(sys);
    }
// for isolated multi cluster only
//////////////////

//////////////////
// for multi cluster
    template<class Tpsoft>
    void driveForMultiCluster(const PS::F64 dt, Tpsoft* _ptcl_soft){
        const PS::S32 n_cluster = n_ptcl_in_cluster_.size();
        /*
          for(PS::S32 ith=0; ith<PS::Comm::getNumberOfThread(); ith++){
          eng_disp_merge_omp_[ith] = 0.0;
          merge_log_omp_[ith].clearSize();
          }
        */
        for(PS::S32 i=0; i<n_cluster; i++){
            const PS::S32 adr_head = n_ptcl_in_cluster_disp_[i];
            const PS::S32 n_ptcl = n_ptcl_in_cluster_[i];
#ifndef ONLY_SOFT
            const PS::S32 n_group = n_group_in_cluster_[i];
            Tpsoft* ptcl_artificial_ptr=NULL;
            if(n_group>0) ptcl_artificial_ptr = &(_ptcl_soft[adr_first_ptcl_arti_in_cluster_[n_group_in_cluster_offset_[i]]]);
#ifdef HARD_DUMP
            assert(hard_dump.size>0);
            hard_dump[0].backup(ptcl_hard_.getPointer(adr_head), n_ptcl, ptcl_artificial_ptr, n_group, dt, manager->ap_manager.n_split);
#endif
            driveForMultiClusterImpl(ptcl_hard_.getPointer(adr_head), n_ptcl, ptcl_artificial_ptr, n_group, dt);
#else
            auto* pi = ptcl_hard_.getPointer(adr_head);
            for (PS::S32 j=0; j<n_ptcl; j++) {
                PS::F64vec dr = pi[j].vel * dt;
                pi[j].pos += dr;
#ifdef CLUSTER_VELOCITY
                pi[j].status.f[0] = pi[j].status.f[1] = 0;
                pi[j].mass_bk.f[0] = pi[j].mass_bk.f[1] = 0;
#endif
                pi[j].calcRSearch(dt);
            }
#endif
//#ifdef HARD_DEBUG
//            if(extra_ptcl.size()>0) fprintf(stderr,"New particle number = %d\n",extra_ptcl.size());
//#endif
//            for (PS::S32 j=0; j<extra_ptcl.size(); j++) {
//                PS::S32 adr = sys.getNumberOfParticleLocal();
//                PS::S32 rank = PS::Comm::getRank();
//                sys.addOneParticle(Tsptcl(extra_ptcl[j],rank,adr));
//            }
        }
    }

    template<class Tpsoft>
    void driveForMultiClusterOMP(const PS::F64 dt, Tpsoft* _ptcl_soft){
        const PS::S32 n_cluster = n_ptcl_in_cluster_.size();
        //PS::ReallocatableArray<PtclH4> extra_ptcl[num_thread];
        //// For test
        //PS::ReallocatableArray<std::pair<PS::S32,PS::S32>> n_sort_list;
        //n_sort_list.resizeNoInitialize(n_cluster);
        //for(PS::S32 i=0; i<n_cluster; i++) {
        //    n_sort_list[i].first = n_ptcl_in_cluster_[i];
        //    n_sort_list[i].second= i;
        //}
        //std::sort(n_sort_list.getPointer(),n_sort_list.getPointer()+n_cluster,[](const std::pair<PS::S32,PS::S32> &a, const std::pair<PS::S32,PS::S32> &b){return a.first<b.first;});
#ifdef OMP_PROFILE        
        const PS::S32 num_thread = PS::Comm::getNumberOfThread();
        PS::ReallocatableArray<PS::F64> time_thread(num_thread);
        PS::ReallocatableArray<PS::S64> num_cluster(num_thread);
        for (PS::S32 i=0; i<num_thread; i++) {
          time_thread[i] = 0;
          num_cluster[i] = 0;
        }
#endif
#pragma omp parallel for schedule(dynamic)
        for(PS::S32 i=0; i<n_cluster; i++){
            const PS::S32 ith = PS::Comm::getThreadNum();
#ifdef OMP_PROFILE
            time_thread[ith] -= PS::GetWtime();
#endif
            //const PS::S32 i   = n_sort_list[k].second;
            const PS::S32 adr_head = n_ptcl_in_cluster_disp_[i];
            const PS::S32 n_ptcl = n_ptcl_in_cluster_[i];
#ifndef ONLY_SOFT
            const PS::S32 n_group = n_group_in_cluster_[i];
            Tpsoft* ptcl_artificial_ptr=NULL;
            if(n_group>0) ptcl_artificial_ptr = &(_ptcl_soft[adr_first_ptcl_arti_in_cluster_[n_group_in_cluster_offset_[i]]]);
#ifdef OMP_PROFILE
            num_cluster[ith] += n_ptcl;
#endif
#ifdef HARD_DUMP
            assert(ith<hard_dump.size);
            hard_dump[ith].backup(ptcl_hard_.getPointer(adr_head), n_ptcl, ptcl_artificial_ptr, n_group, dt, manager->ap_manager.n_split);
#endif

#ifdef HARD_DEBUG_PROFILE
            PS::F64 tstart = PS::GetWtime();
#endif
            driveForMultiClusterImpl(ptcl_hard_.getPointer(adr_head), n_ptcl, ptcl_artificial_ptr, n_group, dt, ith);
#ifdef OMP_PROFILE
            time_thread[ith] += PS::GetWtime();
#endif
#ifdef HARD_DEBUG_PROFILE
            PS::F64 tend = PS::GetWtime();
            std::cerr<<"HT: "<<i<<" "<<ith<<" "<<n_cluster<<" "<<n_ptcl<<" "<<tend-tstart<<std::endl;
#endif
#else
            auto* pi = ptcl_hard_.getPointer(adr_head);
            for (PS::S32 j=0; j<n_ptcl; j++) {
                PS::F64vec dr = pi[j].vel * dt;
                pi[j].pos += dr;
#ifdef CLUSTER_VELOCITY
                pi[j].status.f[0] = pi[j].status.f[1] = 0;
                pi[j].mass_bk.f[0] = pi[j].mass_bk.f[1] = 0;
#endif
                pi[j].calcRSearch(dt);
            }
#endif

        }
//        if (n_cluster>0) {
//            PS::S32 rank = PS::Comm::getRank();
//            for(PS::S32 i=0; i<num_thread; i++) {
//#ifdef OMP_PROFILE        
//                std::cerr<<"thread: "<<i<<"  Hard Time="<<time_thread[i]<<"  n_ptcl="<<num_cluster[i]<<std::endl;
//#endif
//                for (PS::S32 j=0; j<extra_ptcl[i].size(); j++) {
//                    PS::S32 adr = sys.getNumberOfParticleLocal();
//                    sys.addOneParticle(Tsptcl(extra_ptcl[i][j],rank,adr));
//#ifdef HARD_DEBUG
//                    if(sys[adr].id==10477) {
//                        std::cerr<<"Add particle adr="<<adr;
//                        sys[adr].print(std::cerr);
//                        std::cerr<<std::endl;
//                        std::cerr<<" original: ";
//                        extra_ptcl[i][j].print(std::cerr);
//                        std::cerr<<std::endl;
//                    }
//                    if(extra_ptcl[i][j].id<0&&extra_ptcl[i][j].status<0) {
//                        std::cerr<<"Error: extra particle list contain ghost particle! i_thread="<<i<<" index="<<j<<" rank="<<rank<<" adr="<<adr<<std::endl;
//                        abort();
//                    }
//#endif
//                }
//            }
//        }
    }

    //! Find groups and create aritfical particles to sys
    /* @param[in,out] _sys: global particle system
       @param[in]     _dt_tree: tree time step for calculating r_search
     */
    template<class Tsys, class Tptcl>
    void findGroupsAndCreateArtificialParticlesOMP(Tsys & _sys, 
                                                  const PS::F64 _dt_tree) {
        // isolated clusters
        findGroupsAndCreateArtificialParticlesImpl<Tsys, Tptcl>(_sys, 
                                                               ptcl_hard_.getPointer(),
                                                               n_ptcl_in_cluster_,
                                                               n_ptcl_in_cluster_disp_,
                                                               n_group_in_cluster_,
                                                               n_group_in_cluster_offset_,
                                                               adr_first_ptcl_arti_in_cluster_,
                                                               _dt_tree);
    }

#ifdef CLUSTER_VELOCITY
    //! reset group member particle status/mass_bk to zero for search cluster
    /*! update both local and global 
       @param[in,out] _ptcl_soft: global particle
    */
    template <class Tsoft>
    void resetParticleStatus(Tsoft& _ptcl_soft) {
        const PS::S32 n = ptcl_hard_.size();
#pragma omp parallel for schedule(dynamic)
        for(PS::S32 i=0; i<n; i++){
            // to avoid issue in cluster search with velocity
            ptcl_hard_[i].status.f[0]=0.0;
            ptcl_hard_[i].status.f[1]=0.0;
            ptcl_hard_[i].mass_bk.f[0]=0.0;
            ptcl_hard_[i].mass_bk.f[1]=0.0;
            PS::S32 adr = ptcl_hard_[i].adr_org;
#ifdef HARD_DEBUG
            assert(adr>=0);
#endif
            _ptcl_soft[adr].status  = ptcl_hard_[i].status;
            _ptcl_soft[adr].mass_bk = ptcl_hard_[i].mass_bk;
        }
    }

    //! set group member particle status/mass_bk to c.m. data for search cluster
    /*! update both local and global 
       @param[in,out] _ptcl_soft: global particle
    */
    template <class Tsoft>
    void setParticleStatusToCMData(Tsoft& _ptcl_soft) {
        auto& ap_manager = manager->ap_manager;
        const PS::S32 n_cluster = n_ptcl_in_cluster_.size();
#pragma omp parallel for schedule(dynamic)
        for(PS::S32 i=0; i<n_cluster; i++){
            const PS::S32 adr_head = n_ptcl_in_cluster_disp_[i];
            const PS::S32 n_ptcl = n_ptcl_in_cluster_[i];
            const PS::S32 n_group = n_group_in_cluster_[i];
            PtclH4* ptcl_local = ptcl_hard_.getPointer(adr_head);

            PS::S32 n_group_offset = 0;
            if(n_group>0) {
                auto* ptcl_artificial = &(_ptcl_soft[adr_first_ptcl_arti_in_cluster_[n_group_in_cluster_offset_[i]]]);
                for(int k=0; k<n_group; k++) {
                    PS::S32 adr_first_ptcl = k*ap_manager.getArtificialParticleN();
                    auto* pi = &(ptcl_artificial[adr_first_ptcl]);
                    const PS::S32 n_members = ap_manager.getMemberN(pi);
                    auto* pcm = ap_manager.getCMParticles(pi);
                    pcm->mass = pcm->mass_bk.d;
#ifdef ARTIFICIAL_PARTICLE_DEBUG
                    ap_manager.checkConsistence(&ptcl_local[n_group_offset], pi);
#endif
                    for (int j=n_group_offset; j<n_group_offset+n_members; j++) {
                        ptcl_local[j].r_search = std::max(pcm->r_search, ptcl_local[j].r_search);
                        ptcl_local[j].status.f[0] = pcm->vel[0];
                        ptcl_local[j].status.f[1] = pcm->vel[1];
                        ptcl_local[j].mass_bk.f[0]= pcm->vel[2];
                        ptcl_local[j].mass_bk.f[1]= pcm->mass;
                        PS::S32 adr = ptcl_local[j].adr_org;
                        if(adr>=0) {
                            _ptcl_soft[adr].status = ptcl_local[j].status;
                            _ptcl_soft[adr].mass_bk= ptcl_local[j].mass_bk;
                        }
                    }
                    n_group_offset += n_members;
                }
            }
            for (int j=n_group_offset; j<n_ptcl; j++) {
                ptcl_local[j].status.f[0] = 0;
                ptcl_local[j].status.f[1] = 0;
                ptcl_local[j].mass_bk.f[0] = 0;
                ptcl_local[j].mass_bk.f[1] = 0;
                PS::S32 adr = ptcl_local[j].adr_org;
                if(adr>=0) {
                    _ptcl_soft[adr].status = ptcl_local[j].status;
                    _ptcl_soft[adr].mass_bk= ptcl_local[j].mass_bk;
                }
            }
        }
    }
#endif

    //! potential correction for single cluster
    /* The force kernel have self-interaction on the potential contribution, need to be excluded. _sys acc is updated
       @param[in,out] _sys: global particle system, acc is updated
       @param[in] _ptcl_list: list of single particle in _ptcl
       @param[in] _n_ptcl: number of single particles
     */
    template <class Tsys> 
    void correctPotWithCutoffOMP(Tsys& _sys, 
                                 const PS::ReallocatableArray<PS::S32>& _ptcl_list) {
        const PS::S32 n_ptcl = _ptcl_list.size();
#pragma omp parallel for 
        for (int i=0; i<n_ptcl; i++) {
            const PS::S32 k =_ptcl_list[i];
            _sys[k].pot_tot += _sys[k].mass / manager->ap_manager.r_out_base;
//#ifdef HARD_DEBUG
            // status may not be zero after binary disrupted
            // assert(_sys[k].status==0);
//#endif
        }
    }

    //! Soft force correction due to different cut-off function
    /* Use tree neighbor search for local real particles including sending particles.
       Use cluster information correct artificial particles. 
       c.m. force is replaced by the averaged force on orbital particles
       Tidal tensor particle subtract the c.m. acc
       @param[in] _sys: global particle system, acc is updated
       @param[in] _tree: tree for force
       @param[in] _adr_send: particle in sending list of connected clusters
       @param[in] _acorr_flag: flag to do acorr for KDKDK_4TH case
    */
    template <class Tsys, class Tpsoft, class Ttree, class Tepj>
    void correctForceWithCutoffTreeNeighborAndClusterOMP(Tsys& _sys,
                                                         Ttree& _tree,
                                                         const PS::ReallocatableArray<PS::S32>& _adr_send,
                                                         const bool _acorr_flag=false) {
        correctForceWithCutoffTreeNeighborAndClusterImp<Tsys, Tpsoft, Ttree, Tepj>(_sys, _tree, ptcl_hard_.getPointer(), n_ptcl_in_cluster_, n_ptcl_in_cluster_disp_, n_group_in_cluster_, n_group_in_cluster_offset_, adr_first_ptcl_arti_in_cluster_, _adr_send, _acorr_flag);
    }

    //! Soft force correction due to different cut-off function
    /* Use cluster member, first correct for artificial particles, then for cluster member
       c.m. force is replaced by the averaged force on orbital particles
       Tidal tensor particle subtract the c.m. acc
       @param[in] _sys: global particle system, acc is updated
       @param[in] _acorr_flag: flag to do acorr for KDKDK_4TH case
    */
    template <class Tsys>
    void correctForceWithCutoffClusterOMP(Tsys& _sys, const bool _acorr_flag=false) { 
        correctForceWithCutoffClusterImp(_sys, ptcl_hard_.getPointer(), n_ptcl_in_cluster_, n_ptcl_in_cluster_disp_, n_group_in_cluster_, n_group_in_cluster_offset_, adr_first_ptcl_arti_in_cluster_, _acorr_flag);
    }

    //! Correct force due to change over factor change 
    /*!
       @param[in,out] _sys: global particle system, acc is updated
       @param[in] _tree: tree for force, to get neighbor list
       @param[in] _adr_send: for connected case, correct sending list
       @param[in] _n_send: sending particle number
     */
    template <class Tsys, class Ttree, class Tepj>
    void correctForceForChangeOverUpdateOMP(Tsys& _sys, Ttree& _tree, 
                                            const PS::S32*  _adr_send=NULL, const PS::S32 _n_send=0) {
        const PS::S32 n_cluster = i_cluster_changeover_update_.size();
        auto& ap_manager = manager->ap_manager;
#pragma omp parallel for schedule(dynamic)
        for (int i=0; i<n_cluster; i++) {  // i: i_cluster
            PS::S32 i_cluster = i_cluster_changeover_update_[i];
            PS::S32 adr_real_start= n_ptcl_in_cluster_disp_[i_cluster];
            PS::S32 adr_real_end= n_ptcl_in_cluster_disp_[i_cluster+1];
            // artificial particle group number
            PS::S32 n_group = n_group_in_cluster_[i_cluster];
            const PS::S32* adr_first_ptcl_arti = n_group>0? &adr_first_ptcl_arti_in_cluster_[n_group_in_cluster_offset_[i_cluster]] : NULL;
            
            // correction for artificial particles
            for (int j=0; j<n_group; j++) {  // j: j_group
                PS::S32 j_start = adr_first_ptcl_arti[j];
                auto* pj = &(_sys[j_start]);
                
                // loop orbital particles
                auto* porb_j = ap_manager.getOrbitalParticles(pj);
                const PS::S32 n_orb = ap_manager.getOrbitalParticleN();
                for (int k=0; k<n_orb; k++) { 
                    // k: k_ptcl_arti
                    bool changek = porb_j[k].changeover.r_scale_next!=1.0;

                    // loop orbital artificial particle
                    // group
                    for (int kj=0; kj<n_group; kj++) { // group
                        PS::S32 kj_start = adr_first_ptcl_arti[kj];
                        auto* porb_kj = ap_manager.getOrbitalParticles(&_sys[kj_start]);

                        // particle arti orbital
                        if (porb_kj[0].changeover.r_scale_next!=1.0 || changek) {
                            
                            for (int kk=0; kk<n_orb; kk++) {
                                if(&porb_kj[kk]==&porb_j[k]) continue; //avoid same particle
                         
                                calcAccChangeOverCorrection(porb_j[k], porb_kj[kk]);
                            }
                        }
                    }

                    //loop real particle
                    for (int kj=adr_real_start; kj<adr_real_end; kj++) {
                        if (ptcl_hard_[kj].changeover.r_scale_next!=1.0 || changek) {
                            calcAccChangeOverCorrection(porb_j[k], ptcl_hard_[kj]);
                        }
                    }
                }

                // correct c.m. average force
                ap_manager.correctOrbitalParticleForce(pj);
            }

            // correction for real particles
            for (int j=adr_real_start; j<adr_real_end; j++) {
                PS::S64 adr = ptcl_hard_[j].adr_org;
                if(adr>=0) {
                    bool change_i = _sys[adr].changeover.r_scale_next!=1.0;
                    Tepj * ptcl_nb = NULL;
                    PS::S32 n_ngb = _tree.getNeighborListOneParticle(_sys[adr], ptcl_nb);
                    for(PS::S32 k=0; k<n_ngb; k++){
                        if (ptcl_nb[k].id == _sys[adr].id) continue;

                        if (ptcl_nb[k].r_scale_next!=1.0 || change_i) 
                            calcAccChangeOverCorrection(_sys[adr], ptcl_nb[k]);
                    }
                }
                // update changeover
                ptcl_hard_[j].changeover.updateWithRScale();
                if(adr>=0) _sys[adr].changeover.updateWithRScale();
            }
            
        }
#pragma omp parallel for 
        // sending list to other nodes need also be corrected.
        for (int i=0; i<_n_send; i++) {
            PS::S64 adr = _adr_send[i];
            bool change_i = _sys[adr].changeover.r_scale_next!=1.0;
            Tepj * ptcl_nb = NULL;
            PS::S32 n_ngb = _tree.getNeighborListOneParticle(_sys[adr], ptcl_nb);
            for(PS::S32 k=0; k<n_ngb; k++){
                if (ptcl_nb[k].id == _sys[adr].id) continue;
                
                if (ptcl_nb[k].r_scale_next!=1.0 || change_i) 
                    calcAccChangeOverCorrection(_sys[adr], ptcl_nb[k]);
            }
            _sys[adr].changeover.updateWithRScale();
        }
        
    }


    //! Soft force correction due to different cut-off function
    /* Use tree neighbor search for all particles.
       c.m. force is replaced by the averaged force on orbital particles
       Tidal tensor particle subtract the c.m. acc
       @param[in] _sys: global particle system, acc is updated
       @param[in] _tree: tree for force
       @param[in] _adr_ptcl_artificial_start: start address of artificial particle in _sys
    */
    template <class Tsys, class Tpsoft, class Ttree, class Tepj>
    void correctForceWithCutoffTreeNeighborOMP(Tsys& _sys,
                                               Ttree& _tree,
                                               const PS::S32 _adr_ptcl_artificial_start,
                                               const bool _acorr_flag=false) {
        
        correctForceWithCutoffTreeNeighborImp<Tsys, Tpsoft, Ttree, Tepj>(_sys, _tree, _adr_ptcl_artificial_start, _acorr_flag);
    }

};

