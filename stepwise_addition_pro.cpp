/*
 * Copyright (C) 2012 Simon A. Berger
 *
 *  This program is free software; you may redistribute it and/or modify its
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 */
#include <cctype>

#include <algorithm>
#include <functional>
#include <vector>
#include <iostream>
#include <fstream>

#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/numeric/ublas/matrix.hpp>
#include "tree_utils.h"
#include "pairwise_seq_distance.h"
#include "stepwise_align.h"
#include "raxml_interface.h"
#include "sequence_model.h"
#include "pvec.h"
#include "ivymike/time.h"
#include "ivymike/getopt.h"
#include "ivymike/smart_ptr.h"
#include "ivymike/tree_parser.h"
#include "ivymike/tdmatrix.h"
#include "ivymike/algorithm.h"


namespace tree_parser = ivy_mike::tree_parser_ms;

using ivy_mike::tree_parser_ms::ln_pool;
using ivy_mike::tree_parser_ms::lnode;

using sequence_model::tag_aa;
using sequence_model::tag_dna;


namespace ublas = boost::numeric::ublas;


typedef std::vector<unsigned char> sequence;

namespace {
 
    double g_delta = 1.0;
    double g_epsilon = 1.0;
}

class log_odds {
public:

    log_odds( double bg_prob ) : bg_prob_(bg_prob) {}

    inline double operator()( double p ) {
        return std::max( -100.0, log( p / bg_prob_ ));
    }

private:
    const double bg_prob_;
};


class log_odds_viterbi {
    typedef ublas::matrix<double> dmat;
    typedef std::vector<double> dsvec;


    // lof_t: log-odds-float = float type good enough to hold/calculate log-odds scores.
    // 32bit float should be enough
    typedef float lof_t;
    typedef ublas::matrix<lof_t> lomat;

    typedef std::vector<lof_t> losvec;

public:
    log_odds_viterbi( const dmat &state, const dmat &gap, boost::array<double,4> state_freq )
    : ref_state_prob_(state), ref_gap_prob_(gap), ref_len_(state.size2()),
      state_freq_(state_freq),
      neg_inf_( -std::numeric_limits<lof_t>::infinity() ),
      m_(state.size2() + 1),
      d_(state.size2() + 1),
      i_(state.size2() + 1),
      max_matrix_height_(0),
      delta_(g_delta),
      epsilon_(g_epsilon)
    {
        precalc_log_odds();
    }

    void setup( size_t qlen ) {
        assert( ref_gap_prob_.size2() == ref_len_ );



        // init first rows
        std::fill( m_.begin(), m_.end(), 0.0 );
        std::fill( d_.begin(), d_.end(), 0.0 );
        std::fill( i_.begin(), i_.end(), 0.0 /*neg_inf_*/ );

        // init first columns
        m_[0] = 0.0;
        d_[0] = 0.0; /*neg_inf_*/
        i_[0] = 0.0;



    }


    void precalc_log_odds() {
        ref_state_lo_.resize( ref_state_prob_.size1(), ref_state_prob_.size2() );

        for( size_t i = 0; i < 4; ++i ) {
            const ublas::matrix_row<dmat> prow( ref_state_prob_, i );
            ublas::matrix_row<lomat> lorow( ref_state_lo_, i );
            std::transform( prow.begin(), prow.end(), lorow.begin(), log_odds(state_freq_[i]));
        }



//         {
//             odds odds_ngap( 1 - g_gap_freq );
//             odds odds_gap( g_gap_freq );
// 
//             ref_ngap_odds_.resize(ref_gap_prob_.size());
//             ref_gap_odds_.resize(ref_gap_prob_.size());
//             for( size_t i = 0; i < ref_gap_prob_.size(); ++i ) {
//                 ref_ngap_odds_[i] = odds_ngap(1 - ref_gap_prob_[i]);
//                 ref_gap_odds_[i] = odds_gap( ref_gap_prob_[i] );
//             }
//         }
    }

    template<typename T>
    static inline T max3( const T &a, const T &b, const T &c ) {
        return std::max( a, std::max( b, c ));
    }


    double align( const std::vector<uint8_t> &qs ) {
        const size_t qlen = qs.size();

        setup( qlen );

        //dmat ref_state_trans = trans(ref_state_prob_);


        assert( m_.size() == ref_len_ + 1 );




        for( size_t i = 1; i < qlen + 1; ++i ) {
            const int b = qs[i-1];
            //          std::cout << "b: " << b << "\n";

            //const double b_freq = state_freq_.at(b);
            //const ublas::matrix_column<dmat> b_state( ref_state_prob_, b );
            const ublas::matrix_row<lomat> b_state_lo( ref_state_lo_, b );

            //          const ublas::matrix_column<dmat> ngap_prob( ref_gap_prob_, 0 );
            //          const ublas::matrix_column<dmat> gap_prob( ref_gap_prob_, 1 );

            lof_t diag_m = m_[0];
            lof_t diag_d = d_[0];
            lof_t diag_i = i_[0];

            losvec::iterator m0 = m_.begin() + 1;
            losvec::iterator d0 = d_.begin() + 1;
            losvec::iterator i0 = i_.begin() + 1;

            losvec::iterator m1 = m_.begin();
            losvec::iterator d1 = d_.begin();
            losvec::iterator i1 = i_.begin();
            ublas::matrix_row<lomat>::const_iterator bsl = b_state_lo.begin();
//             losvec::iterator rg = ref_gap_odds_.begin();
//             losvec::iterator rng = ref_ngap_odds_.begin();

            auto gap_prob = ref_gap_prob_.begin2();
            
            const losvec::iterator m_end = m_.end();

            for( ; m0 != m_end; m1 = m0++, d1 = d0++, i1 = i0++, ++bsl, ++gap_prob ) {
                //ublas::matrix_row<dmat> a_state(ref_state_prob_, j-1 );
                //ublas::matrix_row<dmat> a_gap(ref_gap_prob_, j-1 );

                //double match_log_odds = log( b_state[j-1] / b_freq );
                //lof_t match_log_odds = b_state_lo[j-1];
                const lof_t match_log_odds = *bsl;



                //lof_t gap_log_odds = ref_gap_lo_[j-1];
                //lof_t ngap_log_odds = ref_ngap_lo_[j-1];
//                 const lof_t gap_odds = *rg;
//                 const lof_t ngap_odds = *rng;


                lof_t p_ngap = *gap_prob;
                lof_t p_gap = *(gap_prob.begin() + 1);
                
//                 lof_t m_log_sum = log(
//                            exp(diag_m) * p_ngap
//                          + exp(diag_d) * p_gap
//                          + exp(diag_i)
//                 );
                lof_t m_log_max = max3<float>(
                           diag_m + log(p_ngap),
                           diag_d + log(p_gap),
                           diag_i 
                );
                
                diag_m = *m0;
                *m0 = m_log_max + match_log_odds;

#if 0
                std::cout << i << " " << j << " " << m_(i,j) << " : " << m_(i-1, j-1) + ngap_log_odds
                        << " " << d_(i-1, j-1) + gap_log_odds << " " << i_(i-1, j-1) + gap_log_odds << " " << match_log_odds << " " << gap_log_odds << " " << ngap_log_odds << " max: " << m_max << "\n";
#endif

                diag_i = *i0;

                // the two 'diags' have already been updated, so they're both actually containing the current 'aboves',
                // which is exactly what we need to calculate the new i

//                 lof_t i_log_sum = log(
//                           exp(diag_m) /** delta_*/
//                         + exp(diag_i) /** epsilon_*/
//                 );
                lof_t i_log_max = std::max(
                          diag_m, /** delta_*/
                          diag_i /** epsilon_*/
                );
                *i0 = i_log_max;

#if 1
//                 lof_t d_log_sum = log(
//                           exp(*m1) /** delta_*/
//                         + exp(*d1) /** epsilon_*/
//                 );
                lof_t d_log_max = std::max(
                          *m1, /** delta_*/
                          *d1 /** epsilon_*/
                );
#else
                lof_t d_log_sum = *m1 + math_approx::log(
                        delta_
                      + math_approx::exp(*d1 - *m1) * epsilon_
                );
#endif
                diag_d = *d0;
                *d0 = d_log_max;


                //lof_t old_m = m_[j];

            }
        }

        return m_.back();
    }

    dmat ref_state_prob_;
    dmat ref_gap_prob_;

    lomat ref_state_lo_;
//     losvec ref_gap_odds_;
//     losvec ref_ngap_odds_;

    const size_t ref_len_;
    const boost::array<double,4> state_freq_;

    const float neg_inf_;

    losvec m_;
    losvec d_;
    losvec i_;

    size_t max_matrix_height_;

    const lof_t delta_;// = log(0.1);
    const lof_t epsilon_;// = log(0.5);

    size_t max_col_;
    size_t max_row_;
    double max_score_;





};



namespace {

template<class pvec_t,typename seq_tag>
class my_adata_gen : public ivy_mike::tree_parser_ms::adata {

    
public:
//     int m_ct;
    my_adata_gen() {

//         std::cout << "my_adata\n";

    }

    virtual ~my_adata_gen() {

//         std::cout << "~my_adata\n";

    }

    
    
    void init_sequence( const sequence &seq ) {
        assert( seq_.empty() );
        
        seq_.assign( seq.begin(), seq.end() );
    }
    
    void update_pvec() {
        pvec_.init2( seq_, sequence_model::model<seq_tag>() );
    }
    
    pvec_t &pvec() {
        return pvec_;
    }
    
    const ublas::matrix<float> &calculate_anc_gap_probs() {
        
        const auto &pgap = pvec_.get_pgap();
        
        anc_probs_.resize(pgap.size1(), pgap.size2());
        
        
        auto oit = anc_probs_.begin2();
        for( auto iit = pgap.begin2(); iit != pgap.end2(); ++iit, ++oit ) {
            double v1 = *iit * (1-pvec_pgap::pgap_model->gap_freq());
            double v2 = *(iit.begin() + 1) * pvec_pgap::pgap_model->gap_freq();
            
            *oit = v1 / (v1 + v2);
            *(oit.begin() + 1) = v2 / (v1 + v2);
            
        }
        
        return anc_probs_;
        
//         for( it = 
        
    }
    
private:
    
    

    
    pvec_t pvec_;
    ublas::matrix<float> anc_probs_;
    
    sequence seq_;
    

};

// inline void newview_parsimony( std::vector<parsimony_state> &p, const std::vector<parsimony_state> &c1, const std::vector<parsimony_state> &c2 ) {
//
// }



// inline std::ostream &operator<<( std::ostream &os, const my_adata &rb ) {
//
//     os << "my_adata: " << rb.m_ct;
// }

template<class ndata_t>
class my_fact : public ivy_mike::tree_parser_ms::node_data_factory {

    virtual ndata_t *alloc_adata() {

        return new ndata_t;
    }

};
   
typedef sequence_model::model<tag_dna> seq_model;
typedef my_adata_gen<pvec_pgap,tag_dna> my_adata;
}


class addition_order {
public:



    addition_order( const scoring_matrix &sm, const std::vector<sequence> &mapped_seqs )
     : used_seqs_( mapped_seqs.size() )
    {
        const size_t num_seqs = mapped_seqs.size();

        std::cout << "size: " << num_seqs << "\n";
        pw_dist_.init_size(num_seqs, num_seqs);


        ivy_mike::tdmatrix<int> out_scores( mapped_seqs.size(), mapped_seqs.size() );

        const size_t num_ali_threads = 4;
        pairwise_seq_distance(mapped_seqs, out_scores, sm, -5, -2, num_ali_threads, 64);
        init_pw_dist_from_msa_score_matrix(out_scores);

    }


    size_t find_next_candidate() {

        if( pw_dist_.size() == 0 ) {
            throw std::runtime_error( "find_next_candidate called with empty pw-dist matrix");
        }

        if( dist_acc_.empty() ) {
            //
            // create initial distance accumulator if it does not exist.
            //
            size_t f = used_seqs_.find_first();

            std::vector<float> dist_sum;

            while( f != used_seqs_.npos ) {
                ivy_mike::odmatrix<float> slice = pw_dist_[f];
                if( dist_sum.empty() ) {
                    dist_sum.assign( slice.begin(), slice.end() );
                } else {
                    std::transform( dist_sum.begin(), dist_sum.end(), slice.begin(), dist_sum.begin(), std::plus<float>() );
                }

                f = used_seqs_.find_next(f);
            }
            dist_acc_.swap( dist_sum );
        }

        float min_dist = 1e8;
        size_t min_element = size_t(-1);
        for( size_t i = 0; i < dist_acc_.size(); i++ ) {
            if( !used_seqs_[i] && dist_acc_[i] < min_dist ) {
                min_dist = dist_acc_[i];
                min_element = i;
            }
        }

        assert( min_element != size_t(-1) || used_seqs_.count() == used_seqs_.size() );

        if( min_element != size_t(-1) ) {

            // update accumulator
            assert( min_element != size_t(-1));
            ivy_mike::odmatrix<float> slice = pw_dist_[min_element];

            assert( slice.size() == dist_acc_.size() );

            // element-wise calculate dist_acc_ = dist_acc_ + slice;
            std::transform( dist_acc_.begin(), dist_acc_.end(), slice.begin(), dist_acc_.begin(), std::plus<float>() );
            used_seqs_[min_element] = true;
        }


        return min_element;

    }


    std::pair<size_t,size_t> first_pair() const {
        return first_pair_;
    }

private:
    void init_pw_dist_from_msa_score_matrix( ivy_mike::tdmatrix<int> &out_scores ) {
        size_t li = -1, lj = -1;
        float lowest_dist = 1e8;
        int min = *(std::min_element( out_scores.begin(), out_scores.end() ));
        int max = *(std::max_element( out_scores.begin(), out_scores.end() ));

        for( size_t i = 0; i < out_scores.size(); i++ ) {

            for( size_t j = 0; j < out_scores[i].size(); j++ ) {

                // three modes for normalizing: min, max and mean
                //const float norm = min( ma[i][i], ma[j][j] );
                //             const float norm = max( ma[i][i], ma[j][j] );
                const float norm = (out_scores[i][j] - min) / float(max-min);


                const float dist = 1.0 - norm;
                pw_dist_[i][j] = dist;

                if( i != j && dist < lowest_dist ) {
                    lowest_dist = dist;
                    li = i;
                    lj = j;
                }

            }

        }

        used_seqs_[li] = used_seqs_[lj] = true;

        first_pair_ = std::make_pair( li, lj );

    }
    ivy_mike::tdmatrix<float> pw_dist_;
    std::vector<float> dist_acc_;
    boost::dynamic_bitset<> used_seqs_;

    std::pair<size_t,size_t> first_pair_;
   // scoring_matrix scoring_matrix_;
};

template<typename K, typename V>
class flat_map {
public:
    flat_map() : sorted_(true) {}
    
    void sort() {
        if( !sorted_ ) {
            std::sort( pairs_.begin(), pairs_.end() );
            sorted_ = true;
        }
    }
    
    void put_fast( const K &key, const V &value ) {
        pairs_.emplace_back( key, value );
        
        sorted_ = false;
    }
    
    void put( const K &key, const V &value ) {
        if( !sorted_ ) {
            throw std::runtime_error( "flat_map::put on unsorted map" );
        }
        
        ipair p{key, value};
        auto lb = std::lower_bound( pairs_.begin(), pairs_.end(), p );
        
        pairs_.emplace(lb, p);
    }

    const V * get( const K &key ) const {
        if( !sorted_ ) {
            throw std::runtime_error( "flat_map::get on unsorted map" );
        }
        auto lb = std::lower_bound( pairs_.begin(), pairs_.end(), ipair{key, V()} );
        
        if( lb == pairs_.end() || lb->key_ != key ) {
            return nullptr;
        } else {
            return &lb->value_;
        }
    }
    
    void reserve( size_t s ) {
        pairs_.reserve(s);
    }
        
    
private:
    struct ipair {
        K key_;
        V value_;
        
        ipair( const K &key, const V &value ) : key_(key), value_(value) {}
        
        
        inline bool operator<( const ipair &other ) const {
            return key_ < other.key_;
        }
        
        
    };
    
    
    std::vector<ipair> pairs_;
    bool sorted_;
};

class sequences {

public:
    sequences( std::istream &is ) : pw_scoring_matrix_( 3, 0 ) {
        assert( is.good() );

//         std::cout << "here\n";
        read_fasta( is, names_, seqs_ );

        std::for_each( seqs_.begin(), seqs_.end(), boost::bind( &sequences::normalize_seq, this, _1) );

        std::vector<std::vector<uint8_t> > qs_mapped;
        mapped_seqs_.reserve(names_.size() );

        // pre-map raw qs seqs to 'state numbers' (=scoring matrix rows/columns)
        for( auto it = seqs_.begin(); it != seqs_.end(); ++it)
        {
            mapped_seqs_.push_back(std::vector< uint8_t >());//(it->size()));
            mapped_seqs_.back().reserve(it->size());
           
            std::for_each( it->begin(), it->end(), scoring_matrix::valid_state_appender<std::vector< uint8_t > >(pw_scoring_matrix_, mapped_seqs_.back() ));

//             std::copy( mapped_seqs_.back().begin(), mapped_seqs_.back().end(), std::ostream_iterator<int>( std::cout, " " ));

            // the raw sequences stored in 'seqs_' filtered for invalid states (e.g., gaps and characters not
            // present in the scoring matrix already. So the members of 'mapped_seqs_' must have the same length.

            assert( mapped_seqs_.back().size() == it->size() );


        }

        
        name_to_index_.reserve(names_.size());
        for( size_t i = 0; i < names_.size(); ++i ) {
            name_to_index_.put_fast( names_[i], i );
        }
        name_to_index_.sort();
        //std::sort( name_to_index_.begin(), name_to_index_.end() );



//        calc_dist_matrix( false );

    }

    const std::vector<sequence> &mapped_seqs() const {
        return mapped_seqs_;
    }
    const std::vector<sequence> &seqs() const {
        return seqs_;
    }


    const sequence &seq_at( size_t i ) const {
        return seqs_.at(i);
    }

    const sequence &mapped_seq_at( size_t i ) const {
        return mapped_seqs_.at(i);
    }

    
    const scoring_matrix &pw_scoring_matrix() const {
        return pw_scoring_matrix_;
    }

    const std::string &name_at( size_t i ) const {
        return names_.at(i);
    }

    
    size_t name_to_index( const std::string &name ) const {
        auto rp = name_to_index_.get(name);
        
        if( rp == nullptr ) {
            std::cerr << "name: " << name << "\n";
            throw std::runtime_error( "name not found" );
        } else {
            return *rp;
        }
        
//         auto it = std::lower_bound( name_to_index_.begin(), name_to_index_.end(), name_to_index_pair( name, size_t(-1) ));
//         
//         if( it == name_to_index_.end() || it->name_ != name ) {
//             std::cerr << "name: " << name << "\n";
//             throw std::runtime_error( "name not found" );
//         } else {
//             return it->index();
//         }
    }

    size_t clone_seq( size_t i, const std::string &name ) {
        std::vector<uint8_t> seq = seqs_.at(i);
        std::vector<uint8_t> mapped_seq = mapped_seqs_.at(i);

        ivy_mike::push_back_swap(seqs_, seq);
        ivy_mike::push_back_swap(mapped_seqs_, mapped_seq);

//        seqs_.push_back( seq );
//        mapped_seqs_.push_back( mapped_seq );
        names_.push_back(name);

//         name_to_index_pair nni( name, names_.size() - 1 );
//         name_to_index_.insert( std::lower_bound( name_to_index_.begin(), name_to_index_.end(), nni ), nni ); 
        
        name_to_index_.put( name, names_.size() - 1 );
        
        return seqs_.size() - 1;
    }

private:

    void normalize_seq( std::vector<uint8_t> &seq ) {
        std::vector<uint8_t> nseq;
      //  nseq.reserve(seq.size());

        for( std::vector<uint8_t>::iterator it = seq.begin(); it != seq.end(); ++it ) {
            uint8_t c = std::toupper(*it);

            if( pw_scoring_matrix_.state_valid(c)) {
                nseq.push_back(c);
            }
        }

        // shrink-to-fit into original vector 'seq'
       // std::vector<uint8_t> tmp( nseq.begin(), nseq.end() );
        seq.swap(nseq);

    }

    class name_to_index_pair {
    public:
        
        
        name_to_index_pair( const std::string &name, size_t idx ) : name_(name), index_(idx) {}
        
        bool operator<( const name_to_index_pair &other ) const {
            return name_ < other.name_;
        }
        
        inline size_t index() const {
            return index_;
        }
        
    private:
        std::string name_;
        size_t index_;
    };
    
    std::vector<std::vector<uint8_t> > seqs_;
    std::vector<std::vector<uint8_t> > mapped_seqs_;
    std::vector<std::string> names_;
    //std::vector<name_to_index_pair> name_to_index_;
    flat_map<std::string,size_t> name_to_index_;

    scoring_matrix pw_scoring_matrix_;


};


static void make_tip( lnode *n, const std::string &name ) {
    assert( n != 0 );
    assert( n->m_data != 0 );


    // check if the node cn be a valid tip: at least two back pointers must be null.
    size_t null_back = 0;
    if( n->back == 0 ) {
        ++null_back;
    }
    if( n->next->back == 0 ) {
        ++null_back;
    }
    if( n->next->next->back == 0 ) {
        ++null_back;
    }

    assert( null_back >= 2 );

    n->m_data->isTip = true;
    n->m_data->setTipName( name );
}

static bool has_node_label( lnode *n ) {
    assert( n != 0 );
    assert( n->m_data != 0 );
    return !n->m_data->nodeLabel.empty();
}

class tree_builder {
public:

    tree_builder( sequences * const seqs, addition_order * const order, ln_pool * const pool )
     : seqs_(*seqs),
       order_(order),
       pool_(pool)
    {
        // build the initial tree. This is mostly based on black magic.

        std::pair<size_t,size_t> first = order->first_pair();

        size_t seqa = first.first; // confusing, isn't it?
        size_t seqb = first.second;

    //    std::copy( seqs.seq_at( seqa ).begin(), seqs.seq_at( seqa ).end(), std::ostream_iterator<char>(std::cout));
    //    std::cout << "\n";
    //    std::copy( seqs.seq_at( seqb ).begin(), seqs.seq_at( seqb ).end(), std::ostream_iterator<char>(std::cout));
    //    std::cout << "\n";


        sequence aligned_a = seqs->seq_at(seqa);
        sequence aligned_b = seqs->seq_at(seqb);

        std::string name_clonea = seqs->name_at( seqa ) + "_clone";
        std::string name_cloneb = seqs->name_at( seqb ) + "_clone";

        size_t seqa_clone = seqs->clone_seq( seqa, name_clonea );
        size_t seqb_clone = seqs->clone_seq( seqb, name_cloneb );

        used_seqs_.resize( seqs->seqs().size(), false );
        used_seqs_[seqa] = true;
        used_seqs_[seqa_clone] = true;
        used_seqs_[seqb] = true;
        used_seqs_[seqb_clone] = true;

        aligned_seqs_.resize( seqs->seqs().size() );

    //    std::copy( aligned_a.begin(), aligned_a.end(), std::ostream_iterator<char>(std::cout));
    //    std::cout << "\n";
    //    std::copy( aligned_b.begin(), aligned_b.end(), std::ostream_iterator<char>(std::cout));
    //    std::cout << "\n";


        lnode *nx = lnode::create( *pool );
        lnode *ny = lnode::create( *pool );
        tree_parser::twiddle_nodes(nx, ny, 1.0, "MOAL", 0 );


        lnode *na1 = lnode::create( *pool );
        lnode *na2 = lnode::create( *pool );

        lnode *nb1 = lnode::create( *pool );
        lnode *nb2 = lnode::create( *pool );

        make_tip( na1, seqs->name_at( seqa ));
        make_tip( na2, name_clonea);
        make_tip( nb1, seqs->name_at( seqb ));
        make_tip( nb2, name_cloneb);

        
        

        tree_parser::twiddle_nodes(na1, nx->next, 1.0, "I1", 0 );
        tree_parser::twiddle_nodes(na2, nx->next->next, 1.0, "I2", 0 );
        tree_parser::twiddle_nodes(nb1, ny->next, 1.0, "I3", 0 );
        tree_parser::twiddle_nodes(nb2, ny->next->next, 1.0, "I4", 0 );

        align_freeshift( seqs->pw_scoring_matrix(), aligned_a, aligned_b, -5, -3 );
        assert( aligned_a.size() == aligned_b.size() );

        
        aligned_seqs_[seqa_clone] = aligned_a;
        aligned_seqs_[seqa].swap( aligned_a );
        aligned_seqs_[seqb_clone] = aligned_b;
        aligned_seqs_[seqb].swap( aligned_b );

        
        
        tree_ = nx;
        

    }
    double calc_gap_freq () {
        size_t ngaps = 0;
        size_t nres = 0;

        size_t idx = used_seqs_.find_first();
        
        //for( std::vector< std::vector< uint8_t > >::const_iterator it = seqs.begin(); it != seqs.end(); ++it ) {
        while( idx != used_seqs_.npos ) {
            const sequence &seq = aligned_seqs_.at(idx);
            assert( !seq.empty() );
            
            nres += seq.size();
            ngaps += std::count_if( seq.begin(), seq.end(), []( unsigned char c ) {return c == '-'; } ); // TODO: this should depend on seq_model
            
            idx = used_seqs_.find_next(idx);
        }

        double rgap = double(ngaps) / nres;
        std::cout << "gap rate: " << ngaps << " " << nres << "\n";
        std::cout << "gap rate: " << rgap << "\n";
        return rgap;
    }
    void init_tree_sequences() {
        // initialize the tip nodes with the aligned sequence data
        
        apply_lnode( tree_, [&]( lnode *n ) {
            if( n->m_data->isTip ) {
                size_t idx = seqs_.name_to_index( n->m_data->tipName );
                const sequence &seq = aligned_seqs_.at(idx);
                
                assert( !seq.empty() );
                std::cout << "init: " << n->m_data->tipName << "\n";
                n->m_data->get_as<my_adata>()->init_sequence( seq );
                n->m_data->get_as<my_adata>()->update_pvec();
            }
        } );
//                 na1->m_data->get_as<my_adata>()->init_sequence(aligned_a);
//         na2->m_data->get_as<my_adata>()->init_sequence(aligned_a);
//         nb1->m_data->get_as<my_adata>()->init_sequence(aligned_b);
//         na2->m_data->get_as<my_adata>()->init_sequence(aligned_b);



    }


    void print_matrix( const ublas::matrix<double> &m ) {
       // for( ; first != last)
    }


    bool insertion_step() {

        std::vector<ublas::matrix<double> > pvecs;
        write_ali_and_tree_for_raxml();

        lnode *n = generate_marginal_ancestral_state_pvecs( *pool_, "sa_tree", "sa_ali", &pvecs );

        tree_ = n;
        init_tree_sequences();
        
        double gap_freq = calc_gap_freq();
        
        auto cand_id = order_->find_next_candidate();
        const auto &cand_seq = seqs_.seq_at(cand_id);
        const auto &cand_mapped_seq = seqs_.mapped_seq_at(cand_id);
        
        
        probgap_model gpm(gap_freq);
        ivy_mike::stupid_ptr_guard<probgap_model> spg( pvec_pgap::pgap_model, &gpm );
        
        
        std::cout << "pvecs: " << pvecs.size() << "\n";

        std::cout << n->backLabel << " " << n->next->backLabel << " " << n->next->next->backLabel << "\n";


//        std::deque<rooted_bifurcation<lnode> > to;
//        rooted_traveral_order_rec(n->next, to, false );
//
//        for( std::deque<rooted_bifurcation<lnode> >::iterator it = to.begin(); it != to.end(); ++it ) {
//            std::cout << *it << "\n";
//        }


//         for( auto it = pvecs.begin(); it != pvecs.end(); ++it ) {
//             std::cout << "vec:\n";
//             
//             
//             
//             for( auto it1 = it->begin2(); it1 != it->end2(); ++it1 ) {
//                 std::transform( it1.begin(), it1.end(), std::ostream_iterator<double>( std::cout, "\t" ), [](double x) {return x; /*std::max(1.0,-log(x));*/});
//                 std::cout << "\n";
//             }
//             
//         }
        
        
        std::vector<lnode *> labelled_nodes;
        iterate_lnode(n, back_insert_ifer( labelled_nodes, has_node_label ));

        
        
        std::cout << "num labelled: " << labelled_nodes.size() << "\n";

        std::sort( labelled_nodes.begin(), labelled_nodes.end(), [](lnode *n1, lnode *n2){return n1->m_data->nodeLabel < n2->m_data->nodeLabel;} ); 
        
        
        lnode *virtual_root = lnode::create( *pool_ );
        
        
        bool incremental = false;
        for( lnode *np : labelled_nodes ) {

            ivy_mike::tree_parser_ms::splice_with_rollback swr( np, virtual_root );
            
            
            std::deque<rooted_bifurcation<lnode>> rto;
            rooted_traveral_order_rec( virtual_root, rto, incremental );
            incremental = true;
            for( auto it = rto.begin(); it != rto.end(); ++it ) {
                my_adata *p = it->parent->m_data->get_as<my_adata>();
                my_adata *c1 = it->child1->m_data->get_as<my_adata>();
                my_adata *c2 = it->child2->m_data->get_as<my_adata>();
                 std::cout << "newview: " << *it << " " << p << " " << it->parent << "\n";
                //         std::cout << "tip case: " << (*it) << "\n";
                pvec_pgap::newview(p->pvec(), c1->pvec(), c2->pvec(), it->child1->backLen, it->child2->backLen, it->tc);
                
            }
            
             std::cout << "vr: " << *virtual_root->m_data << " " << virtual_root << "\n";
            
            const pvec_pgap &rpp = virtual_root->m_data->get_as<my_adata>()->pvec();
            const boost::numeric::ublas::matrix< double > &pm = rpp.get_gap_prob();
            const auto &anc_gap = virtual_root->m_data->get_as<my_adata>()->calculate_anc_gap_probs();
            
            
            std::cout << pm.size1() << " " << pm.size2() << "\n";
            
//             for( auto it1 = pm.begin2(); it1 != pm.end2(); ++it1 ) {
//                 std::transform( it1.begin(), it1.end(), std::ostream_iterator<double>( std::cout, "\t" ), [](double x) {return x; /*std::max(1.0,-log(x));*/});
//                 std::cout << "\n";
//             }
//             std::cout << "===============\n";
//             for( auto it1 = anc_gap.begin2(); it1 != anc_gap.end2(); ++it1 ) {
//                 std::transform( it1.begin(), it1.end(), std::ostream_iterator<double>( std::cout, "\t" ), [](double x) {return x; /*std::max(1.0,-log(x));*/});
//                 std::cout << "\n";
//             }
            
            size_t node_label = size_t(-1);
            {
                std::stringstream ss( np->m_data->nodeLabel );
                ss >> node_label;
            }
            assert( node_label != size_t(-1) );
            
            std::cout << "node label: " << node_label << "\n";
            auto const & anc_state = pvecs.at( node_label );
            
            
            boost::array<double,4> bg_state{0.25, 0.25, 0.25, 0.25};
            log_odds_viterbi lov(anc_state, anc_gap, bg_state );
            auto score = lov.align(cand_mapped_seq);
            std::cout << "cand:\n";
            std::copy( cand_seq.begin(), cand_seq.end(), std::ostream_iterator<char>(std::cout));
            std::cout << "\n";
            std::cout << "score: " << score << "\n";
//             std::cout << np->m_data->nodeLabel << " " << np->m_data->isTip << "\n";
//             
//             
//             tree_parser::print_newick( np, std::cout, false );
//             std::cout << "\n";
        }
        

//         lnode *tn = labelled_nodes[1];
//         std::cout << tn->m_data->nodeLabel << "\n";
// 
//         if( tn->m_data->isTip ) {
//             std::cout << "is tip\n";
//             assert( tn->back != 0 );
//             
//             tn = tn->back;
//         }
//         
//         std::deque<rooted_bifurcation<lnode> > to;
//         rooted_traveral_order_rec( tn, to, false );
// 
//         for( auto it = to.begin(); it != to.end(); ++it ) {
//             std::cout << *it << "\n";
//         }



//         size_t next_candidate = order_->find_next_candidate();


        pool_->mark(tree_);
        pool_->sweep();
        
        return true;
    }

private:

    void write_ali_and_tree_for_raxml() {
        {
            std::ofstream os( "sa_tree" );
            tree_parser::print_newick( tree_, os );
        }



        std::ofstream os( "sa_ali" );


        size_t pos = used_seqs_.find_first();

        os << used_seqs_.count() << " " << aligned_seqs_.at(pos).size() << "\n";

        while( pos != used_seqs_.npos ){
            assert( pos < aligned_seqs_.size() );

            os << seqs_.name_at(pos) << " ";
            std::copy( aligned_seqs_[pos].begin(), aligned_seqs_[pos].end(), std::ostream_iterator<char>(os));
            os << "\n";

            pos = used_seqs_.find_next(pos);
        }

    }


    const sequences &seqs_;
    addition_order * const order_;
    ln_pool * const pool_;
    boost::dynamic_bitset<> used_seqs_;
    lnode * tree_;

    std::vector<sequence> aligned_seqs_;

    
    
};

//void insertion_loop( sequences *seqs, addition_order *order, ln_pool * const pool ) {
//
//    std::pair<size_t,size_t> first = order->first_pair();
//
//    size_t seqa = first.first; // confusing, isn't it?
//    size_t seqb = first.second;
//
////    std::copy( seqs.seq_at( seqa ).begin(), seqs.seq_at( seqa ).end(), std::ostream_iterator<char>(std::cout));
////    std::cout << "\n";
////    std::copy( seqs.seq_at( seqb ).begin(), seqs.seq_at( seqb ).end(), std::ostream_iterator<char>(std::cout));
////    std::cout << "\n";
//
//
//    sequence aligned_a = seqs->seq_at(seqa);
//    sequence aligned_b = seqs->seq_at(seqb);
//
//    std::string name_clonea = seqs->name_at( seqa ) + "_clone";
//    std::string name_cloneb = seqs->name_at( seqb ) + "_clone";
//
//    size_t seqa_clone = seqs->clone_seq( seqa, name_clonea );
//    size_t seqb_clone = seqs->clone_seq( seqb, name_cloneb );
//
//
//
//
////    std::copy( aligned_a.begin(), aligned_a.end(), std::ostream_iterator<char>(std::cout));
////    std::cout << "\n";
////    std::copy( aligned_b.begin(), aligned_b.end(), std::ostream_iterator<char>(std::cout));
////    std::cout << "\n";
//
//
//    lnode *nx = lnode::create( *pool );
//    lnode *ny = lnode::create( *pool );
//    tree_parser::twiddle_nodes(nx, ny, 1.0, "MOAL", 0 );
//
//
//    lnode *na1 = lnode::create( *pool );
//    lnode *na2 = lnode::create( *pool );
//
//    lnode *nb1 = lnode::create( *pool );
//    lnode *nb2 = lnode::create( *pool );
//
//    make_tip( na1, seqs->name_at( seqa ));
//    make_tip( na2, name_clonea);
//    make_tip( nb1, seqs->name_at( seqb ));
//    make_tip( nb2, name_cloneb);
//
//
//    tree_parser::twiddle_nodes(na1, nx->next, 1.0, "I1", 0 );
//    tree_parser::twiddle_nodes(na2, nx->next->next, 1.0, "I2", 0 );
//    tree_parser::twiddle_nodes(nb1, ny->next, 1.0, "I3", 0 );
//    tree_parser::twiddle_nodes(nb2, ny->next->next, 1.0, "I4", 0 );
//
//    {
//        std::ofstream os( "sa_tree" );
//        tree_parser::print_newick( nx, os );
//    }
//
//    align_freeshift( seqs->pw_scoring_matrix(), aligned_a, aligned_b, -5, -3 );
//    assert( aligned_a.size() == aligned_b.size() );
//    {
//        std::ofstream os( "sa_ali" );
//        os << "4 " << aligned_a.size() << "\n";
//
//
//        os << seqs->name_at( seqa ) << " ";
//        std::copy( aligned_a.begin(), aligned_a.end(), std::ostream_iterator<char>(os));
//        os << "\n";
//        os << seqs->name_at( seqa_clone ) << " ";
//        std::copy( aligned_a.begin(), aligned_a.end(), std::ostream_iterator<char>(os));
//        os << "\n";
//
//
//
//
//        os << seqs->name_at( seqb ) << " ";
//        std::copy( aligned_b.begin(), aligned_b.end(), std::ostream_iterator<char>(os));
//        os << "\n";
//        os << seqs->name_at( seqb_clone ) << " ";
//        std::copy( aligned_b.begin(), aligned_b.end(), std::ostream_iterator<char>(os));
//        os << "\n";
//
//
//    }
//
//
////    size_t next;
////    while( (next = order->find_next_candidate()) != size_t(-1)) {
////        std::cout << "next: " << next << "\n";
////        std::copy( seqs->seq_at( next ).begin(), seqs->seq_at( next ).end(), std::ostream_iterator<char>(std::cout));
////        std::cout << "\n";
////
////    }
//}

int main( int argc, char *argv[] ) {
//    {
//        size_t n = 1024 * 1024 * 1024;
//        boost::dynamic_bitset<> bs( n, false );
//
//        for( size_t i = 0; i < n; ++i ) {
//            if( std::rand() < RAND_MAX / 2 ) {
//                bs.set( i, true );
//            }
//        }
//
//
//        ivy_mike::timer t1;
//        size_t x = 0;
//        size_t pos = bs.find_first();
//        while( pos != bs.npos) {
//            x += pos;
//            pos = bs.find_next(pos);
//        }
//
//
//        std::cout << "elapsed: " << t1.elapsed() << "\n";
//
//        return 0;
//    }



    ivy_mike::getopt::parser igp;
    std::string opt_seq_file;

    int num_cores = boost::thread::hardware_concurrency();

    int opt_num_ali_threads;
    int opt_num_nv_threads;
    bool opt_load_scores;

    igp.add_opt('h', false );
    igp.add_opt('f', ivy_mike::getopt::value<std::string>(opt_seq_file) );
    igp.add_opt('j', ivy_mike::getopt::value<int>(opt_num_ali_threads).set_default(num_cores) );
    igp.add_opt('k', ivy_mike::getopt::value<int>(opt_num_nv_threads).set_default(1) );
    igp.add_opt('l', ivy_mike::getopt::value<bool>(opt_load_scores, true).set_default(false) );
    bool ret = igp.parse(argc, argv);

    if( igp.opt_count('h') != 0 || !ret ) {
        std::cout <<
        "  -h        print help message\n";
        return 0;

    }


    if( igp.opt_count('f') != 1 ) {
        std::cerr << "missing option -f\n";
#ifndef WIN32 // hack. make it easier to start inside visual studio
        return 0;
#endif
        opt_seq_file = "test_218/218.fa";
    }

    const char *filename = opt_seq_file.c_str();



    std::map<std::string, std::vector<uint8_t> >out_msa1;
//    sptr::shared_ptr<ln_pool> pool;//(new ln_pool(std::auto_ptr<node_data_factory>(new my_fact()) ));

    ln_pool pool( std::auto_ptr<ivy_mike::tree_parser_ms::node_data_factory>(new my_fact<my_adata>));



    std::ifstream sis( filename );
    sequences seqs( sis );

    addition_order order( seqs.pw_scoring_matrix(), seqs.mapped_seqs() );

//    insertion_loop( &seqs, &order, &pool );
    tree_builder builder( &seqs, &order, &pool );




    while(true) {
        bool done = builder.insertion_step();
        if( done ) {
            break;
        }
    }

//    builder.write_ali_and_tree_for_raxml();


}
