#ifndef REEBER_PESISTENT_INTEGRAL_BLOCK_H
#define REEBER_PESISTENT_INTEGRAL_BLOCK_H

#include <cassert>

#include <boost/range/combine.hpp>
#include <boost/foreach.hpp>

typedef MergeTreeBlock::MergeTree                                   MergeTree;
typedef MergeTreeBlock::MergeTree::Neighbor                         Neighbor;
typedef MergeTree::Node                                             MergeTreeNode;

struct MinIntegral
{
                     MinIntegral() : min_vtx(0), min_val(0), integral(0), n_cells(0), add_sums()  {}
                     MinIntegral(const Neighbor min_node_, size_t n_add_sums = 0,  Real integral_ = 0, size_t n_cells_ = 0) :
                         min_vtx(min_node_->vertex), min_val(min_node_->value), integral(integral_), n_cells(n_cells_), add_sums(n_add_sums, 0) {}

    void             combine(const MinIntegral& other)
    {
        integral += other.integral;
        n_cells += other.n_cells;
        typedef boost::tuple<Real&, Real> Realref_Real_tuple;
        assert(add_sums.size() == other.add_sums.size());
        BOOST_FOREACH(Realref_Real_tuple t, boost::combine(add_sums, other.add_sums))
            t.get<0>() += t.get<1>();
        append(other);
    }

#ifdef REEBER_PERSISTENT_INTEGRAL_TRACE_VTCS
    void             append(const MinIntegral& other)               { vertices.insert(vertices.end(), other.vertices.begin(), other.vertices.end()); }
    void             push_back(const MergeTreeNode::ValueVertex& v) { vertices.push_back(v); }
#else
    void             append(const MinIntegral& other)               { }
    void             push_back(const MergeTreeNode::ValueVertex& v) { }
#endif

    bool             operator<(const MinIntegral& other) const     { return min_val < other.min_val || (min_val == other.min_val && min_vtx < other.min_vtx); }
    bool             operator>(const MinIntegral& other) const     { return min_val > other.min_val || (min_val == other.min_val && min_vtx > other.min_vtx); }

    MergeTreeNode::Vertex   min_vtx;
    MergeTreeNode::Value    min_val;
    Real                    integral;
    size_t                  n_cells;
    std::vector<Real>       add_sums;
#ifdef REEBER_PERSISTENT_INTEGRAL_TRACE_VTCS
    std::vector<MergeTreeNode::ValueVertex> vertices;
#endif
};

namespace diy {
    template<>
    struct Serialization<MinIntegral>
    {
        static void      save(diy::BinaryBuffer& bb, const MinIntegral &mi)
        {
            diy::save(bb, mi.min_vtx);
            diy::save(bb, mi.min_val);
            diy::save(bb, mi.integral);
            diy::save(bb, mi.n_cells);
            diy::save(bb, mi.add_sums);
#ifdef REEBER_PERSISTENT_INTEGRAL_TRACE_VTCS
            diy::save(bb, mi.vertices);
#endif
        }
        static void      load(diy::BinaryBuffer& bb, MinIntegral &mi)
        {
            diy::load(bb, mi.min_vtx);
            diy::load(bb, mi.min_val);
            diy::load(bb, mi.integral);
            diy::load(bb, mi.n_cells);
            diy::load(bb, mi.add_sums);
#ifdef REEBER_PERSISTENT_INTEGRAL_TRACE_VTCS
            diy::load(bb, mi.vertices);
#endif
        }
    };
}

struct PersistentIntegralBlock
{
    typedef std::vector<MinIntegral>                                MinIntegralVector;
    typedef std::vector<Real>                                       Size;
    int                                                             gid;
    Size                                                            cell_size;
    MergeTreeBlock::Box                                             local;
    MergeTreeBlock::Box                                             global;
    MinIntegralVector                                               persistent_integrals;

                     PersistentIntegralBlock()                      { }
                     PersistentIntegralBlock(const MergeTreeBlock& mtb) :
                         gid(mtb.gid), cell_size(mtb.cell_size), local(mtb.local),
                         global(mtb.global), persistent_integrals() { }
    void             add_integral(const MinIntegral& mi)            { persistent_integrals.push_back(mi); }
    static void*     create()                                       { return new PersistentIntegralBlock; }
    static void      destroy(void*b)  { delete static_cast<PersistentIntegralBlock*>(b); }
    static void      save(const void *b, diy::BinaryBuffer& bb)     { diy::save(bb, *static_cast<const PersistentIntegralBlock*>(b)); }
    static void      load(      void *b, diy::BinaryBuffer& bb)     { diy::load(bb, *static_cast<PersistentIntegralBlock*>(b)); }
};

#endif

