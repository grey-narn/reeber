#include <iostream>
#include <memory>
#include <string>

#include <boost/algorithm/string/split.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/range/combine.hpp>

#include <opts/opts.h>
#include <dlog/log.h>
#include <dlog/stats.h>

#include <diy/decomposition.hpp>
#include <diy/reduce.hpp>
#include <diy/partners/swap.hpp>
#include <diy/io/block.hpp>

#include <reeber/format.h>

#include "reader-interfaces.h"

#include "merge-tree-block.h"
#include "persistent-integral-block.h"

typedef diy::RegularDecomposer<diy::DiscreteBounds>                 Decomposer;
typedef MergeTreeBlock::Vertex                                      Vertex;
typedef MergeTreeBlock::Value                                       Value;

class TreeTracer
{
    public:
        typedef std::map<MergeTreeNode::Vertex, MinIntegral>        MinIntegralMap;
        typedef std::vector<diy::DiscreteBounds>                    Boxes;

    public:
                    TreeTracer(Boxes* boxes_, const Decomposer& decomposer_, diy::Master& pi_master_, Real m_, Real t_, std::vector<std::string> avg_fn_list_, std::string density_fn_, bool density_weighted_) :
                         decomposer(decomposer_), pi_master(pi_master_), m(m_), t(t_), density_reader(0), density_weighted(density_weighted_),
                         boxes(boxes_)
        {

            diy::mpi::communicator& world = pi_master_.communicator();

            for(const std::string& fn : avg_fn_list_)
                avg_var_readers.push_back(std::shared_ptr<Reader>(Reader::create(fn, world)));

            if (!density_fn_.empty())
                density_reader = std::shared_ptr<Reader>(Reader::create(density_fn_, world));
        }

        void        operator()(void *b, const diy::ReduceProxy& rp, const diy::RegularSwapPartners& partners) const
        {
            MergeTreeBlock &block = *static_cast<MergeTreeBlock*>(b);

            MinIntegralMap mi_map;
            if (rp.in_link().size() == 0)
            {
                std::vector<MergeTreeBlock::OffsetGrid*> add_data;
                for(std::shared_ptr<Reader> reader : avg_var_readers)
                    add_data.push_back(reader->read(block.core));

                MergeTreeBlock::OffsetGrid::GridProxy gp(0, block.global.shape());
                MergeTreeBlock::OffsetGrid *density_data = density_reader ? density_reader->read(block.core) : 0;
                trace(block.mt.find_root(), block, gp, add_data, density_data, mi_map);

                for(MergeTreeBlock::OffsetGrid* og : add_data)
                    delete og;
                delete density_data;
            }
            else
            {
                for (int i = 0; i < rp.in_link().size(); ++i)
                {
                    int gid = rp.in_link().target(i).gid;

                    // ALTERNATIVE: diy::MemoryBiffer& incoming = rp.incoming(gid);
                    while (rp.incoming(gid)) // ALTERNATIVE: while (incoming)
                    {
                        // ALTERNATIVE: diy::load(incoming, mi);
                        MinIntegral mi;
                        rp.dequeue(gid, mi);
                        if (mi_map.find(mi.min_vtx) == mi_map.end())
                            mi_map[mi.min_vtx] = mi;
                        else
                            mi_map[mi.min_vtx].combine(mi);
                    }
                }
            }

            if (rp.out_link().size() != 0)
            {
                diy::DiscreteBounds& box = (*boxes)[rp.master()->lid(rp.gid())];

                int         group_size  = rp.out_link().size();     // number of outbound partners
                unsigned    round       = rp.round();               // current round number
                int         dim         = partners.dim(round);      // current dimension along which groups are formed
                int         width       = (box.max[dim] - box.min[dim] + 1)/group_size;

                for(const MinIntegral& mi : mi_map | reeber::range::map_values)
                {
                    int dest_gid = decomposer.point_to_gid(block.global.position(mi.min_vtx));
                    Decomposer::DivisionsVector coords;
                    decomposer.gid_to_coords(dest_gid, coords);

                    int pos = (coords[dim] - box.min[dim])/width;
                    rp.enqueue(rp.out_link().target(pos), mi);
                }

                // adjust our box boundaries
                Decomposer::DivisionsVector coords;
                decomposer.gid_to_coords(rp.gid(), coords);
                int pos = (coords[dim] - box.min[dim])/width;

                box.max[dim] = box.min[dim] + (pos+1)*width - 1;
                box.min[dim] = box.min[dim] + pos*width;
            } else
            {
                PersistentIntegralBlock *pi_block = new PersistentIntegralBlock(block);
                for (MinIntegralMap::const_iterator it = mi_map.begin(); it != mi_map.end(); ++it)
                    pi_block->add_integral(it->second);
                pi_master.add(rp.gid(), pi_block, new diy::Link);
            }
        }

        void        trace(Neighbor n, const MergeTreeBlock& block, const MergeTreeBlock::OffsetGrid::GridProxy& gp, const std::vector<MergeTreeBlock::OffsetGrid*>& add_data, MergeTreeBlock::OffsetGrid* density_data, MinIntegralMap& mi_map) const
        {
            for(Neighbor child : n->children)
            {
                if (block.mt.cmp(t, child->value))                                   // still too high
                    trace(child, block, gp, add_data, density_data, mi_map);
                else                                                                 // crossing the threshold
                {
                    MinIntegral mi = integrate(child, block, gp, add_data, density_data);
                    if (block.mt.cmp(m, mi.min_val))
                        continue;

                    if (mi.integral == 0)                                            // non-local extrema are redundant
                        continue;

                    mi_map[mi.min_vtx] = mi;
                }
            }
        }

        MinIntegral integrate(Neighbor n, const MergeTreeBlock& block, const MergeTreeBlock::OffsetGrid::GridProxy& gp, const std::vector<MergeTreeBlock::OffsetGrid*>& add_data, MergeTreeBlock::OffsetGrid *density_data) const
        {

            MinIntegral mi_res(n, add_data.size());

            // Find contribution from n
            if (block.core.contains(n->vertex))
            {
                mi_res.integral += n->value * block.cell_size[0] * block.cell_size[1] * block.cell_size[2];
                ++mi_res.n_cells;
                for(auto t : boost::combine(mi_res.add_sums, add_data))
                {
                    Real new_val = (*t.get<1>())(n->vertex);
                    if (density_data)
                        new_val /= (*density_data)(n->vertex);
                    if (density_weighted)
                        new_val *= n->value * block.cell_size[0] * block.cell_size[1] * block.cell_size[2];
                    t.get<0>() += new_val;
                }
                mi_res.push_back(MergeTreeNode::ValueVertex(n->value, n->vertex));
            }

            for(const MergeTree::Node::ValueVertex& x : n->vertices)
                if (block.core.contains(x.second) && block.mt.cmp(x.first, t))
                {
                    mi_res.integral += x.first * block.cell_size[0] * block.cell_size[1] * block.cell_size[2];
                    ++mi_res.n_cells;
                    for(auto t : boost::combine(mi_res.add_sums, add_data))
                    {
                        Real new_val = (*t.get<1>())(x.second);
                        if (density_data)
                            new_val /= (*density_data)(x.second);
                        if (density_weighted)
                            new_val *= x.first * block.cell_size[0] * block.cell_size[1] * block.cell_size[2];
                        t.get<0>() += new_val;
                    }
                    mi_res.push_back(x);
                }

            // Find contribution from children + min
            for(Neighbor child : n->children)
            {
                MinIntegral mi = integrate(child, block, gp, add_data, density_data);
                if (block.mt.cmp(mi, mi_res))
                {
                    mi_res.min_val = mi.min_val;
                    mi_res.min_vtx = mi.min_vtx;
                }
                mi_res.combine(mi);
            }

            return mi_res;
        }

    private:
        const Decomposer&                      decomposer;
        diy::Master&                           pi_master;
        Real                                   m;
        Real                                   t;
        std::vector< std::shared_ptr<Reader> > avg_var_readers;
        std::shared_ptr<Reader>                density_reader;
        bool                                   density_weighted;
        std::vector<diy::DiscreteBounds>*  boxes;
};

bool vv_cmp(const MergeTreeNode::ValueVertex& a, const MergeTreeNode::ValueVertex& b)
{
    return a.second < b.second;
}

class OutputIntegrals {
    public:
                    OutputIntegrals(std::string outfn_, bool density_weighted_, bool verbose_) : outfn(outfn_), density_weighted(density_weighted_), verbose(verbose_) {}
       void         operator()(PersistentIntegralBlock* b, const diy::Master::ProxyWithLink& cp) const
       {
           PersistentIntegralBlock&  block = *b;

           std::string   dgm_fn = fmt::format("{}-b{}.comp", outfn, block.gid);
           std::ofstream ofs(dgm_fn.c_str());
 
           MergeTreeBlock::OffsetGrid::GridProxy gp(0, block.global.shape());
           for(MinIntegral &mi : block.persistent_integrals)
           {
               Vertex v = block.global.position(mi.min_vtx);
               ofs << v[0] * block.cell_size[0] << " " << v[1] * block.cell_size[1] << " " << v[2] * block.cell_size[2] << " ";
               if (verbose)
                   ofs << v[0] << "x" << v[1] << "x" << v[2] << " (" << mi.min_vtx << ") ";
               ofs <<  mi.integral;
               if (verbose)
                   ofs << " " << mi.n_cells;
               for(Real sum : mi.add_sums)
                   ofs << " " << sum / (density_weighted ? mi.integral : mi.n_cells);
               ofs << std::endl;
#ifdef REEBER_PERSISTENT_INTEGRAL_TRACE_VTCS
               std::sort(mi.vertices.begin(), mi.vertices.end(), vv_cmp);
               for (std::vector< MergeTreeNode::ValueVertex >::const_iterator it = mi.vertices.begin(); it != mi.vertices.end(); ++it)
                   ofs << "   " << it->second << " (" << block.global.position(it->second) <<  ")" << std::endl;
#if 0
               // Consistency check for debugging
               for (size_t i =0; i < mi.vertices.size()-1; ++i)
                   if (mi.vertices[i].second == mi.vertices[i+1].second)
                       LOG_SEV(info) << "Duplicate vertex " << mi.vertices[i].second << " in component " << mi.min_vtx;
#endif
#endif
           }
       }

    private:
        std::string outfn;
        bool        density_weighted;
        bool        verbose;
};

int main(int argc, char** argv)
{
    diy::mpi::environment   env(argc, argv);
    diy::mpi::communicator  world;
#ifdef REEBER_USE_BOXLIB_READER
    reeber::io::BoxLib::environment boxlib_env(argc, argv, world);
#endif

    using namespace opts;

    std::string prefix      = "./DIY.XXXXXX";
    int         in_memory   = -1;
    int         threads     = 1;
    int         k           = 2;
    Real        m           = 200;
    Real        t           = 82;

    std::string profile_path;
    std::string log_level = "info";
    std::string avg_fn_str = "";
    std::string density_fn = "";

    Options ops(argc, argv);
    ops
        >> Option('m', "memory",    in_memory,    "maximum blocks to store in memory")
        >> Option('j', "jobs",      threads,      "threads to use during the computation")
        >> Option('k', "k",         k,            "use k-ary swap")
        >> Option('s', "storage",   prefix,       "storage prefix")
        >> Option('p', "profile",   profile_path, "path to keep the execution profile")
        >> Option('l', "log",       log_level,    "log level")
        >> Option('x', "max",       m,            "maximum threshold")
        >> Option('i', "iso",       t,            "isofind threshold")
        >> Option('f', "mean",      avg_fn_str,   "list of additionals files/variables to average separated by ','")
        >> Option('q', "quotient",  density_fn,   "divide by density in file")
    ;
    bool absolute         = ops >> Present('a', "absolute", "use absolute values for thresholds (instead of multiples of mean)");
    bool verbose          = ops >> Present('v', "verbose",  "verbose output: logical coordiantes and number of cells");
    bool density_weighted = ops >> Present('w', "weight",   "compute density-weighted averages");
    bool split            = ops >> Present(     "split",    "use split IO");

    std::string infn, outfn;
    if (  ops >> Present('h', "help", "show help message") ||
        !(ops >> PosOption(infn) >> PosOption(outfn)))
    {
        if (world.rank() == 0)
            fmt::print("Usage: {} IN.lgt OUT.pi\n{}", argv[0], ops);
        return 1;
    }

    std::vector<std::string> avg_fn_list;
    if (!avg_fn_str.empty())
        boost::split(avg_fn_list, avg_fn_str, std::bind1st(std::equal_to<char>(), ','));

    dlog::add_stream(std::cerr, dlog::severity(log_level))
        << dlog::stamp() << dlog::aux_reporter(world.rank()) << dlog::color_pre() << dlog::level() << dlog::color_post() >> dlog::flush();

    std::ofstream   profile_stream;
    if (profile_path == "-")
        dlog::prof.add_stream(std::cerr);
    else if (!profile_path.empty())
    {
        std::string profile_fn = fmt::format("{}-r{}.prf", profile_path, world.rank());
        profile_stream.open(profile_fn.c_str());
        dlog::prof.add_stream(profile_stream);
    }

    world.barrier();
    dlog::Timer timer;
    LOG_SEV_IF(world.rank() == 0, info) << "Starting computation";

    diy::FileStorage            storage(prefix);

    diy::Master                 mt_master(world,
                                          threads,
                                          in_memory,
                                          &MergeTreeBlock::create,
                                          &MergeTreeBlock::destroy,
                                          &storage,
                                          &MergeTreeBlock::save,
                                          &MergeTreeBlock::load);

    diy::Master                 pi_master(world,
                                          threads,
                                          in_memory);

    diy::ContiguousAssigner     assigner(world.size(), 0);

    // load the trees
    LOG_SEV_IF(world.rank() == 0, debug) << "Reading blocks from " << infn;
    if (!split)
        diy::io::read_blocks(infn, world, assigner, mt_master);
    else
        diy::io::split::read_blocks(infn, world, assigner, mt_master);
    LOG_SEV_IF(world.rank() == 0, info) << "Blocks read: " << mt_master.size();

    world.barrier();
    LOG_SEV_IF(world.rank() == 0, info) << "Time to read data:                    " << dlog::clock_to_string(timer.elapsed());
    timer.restart();

    // get the domain bounds from any block that's in memory (they are all the same) and set up a decomposer
    MergeTreeBlock::Box global = static_cast<MergeTreeBlock*>(((const diy::Master&) mt_master).block(mt_master.loaded_block()))->global;
    diy::DiscreteBounds domain {3};
    for (unsigned i = 0; i < 3; ++i)
    {
        domain.min[i] = global.from()[i];
        domain.max[i] = global.to()[i];
    }
    diy::RegularDecomposer<diy::DiscreteBounds>     decomposer(3, domain, assigner.nblocks(), Decomposer::BoolVector(3, true));

    // Compute average
    if (!absolute)
    {
        mt_master.foreach(&MergeTreeBlock::compute_average);
        mt_master.exchange();

        const diy::Master::ProxyWithLink& proxy = mt_master.proxy(mt_master.loaded_block());
        double mean = proxy.get<double>() / proxy.get<size_t>();
        m *= mean;
        t *= mean;

        LOG_SEV_IF(world.rank() == 0, info) << "Average value is " << mean << ". Using isofind threshold of " << t << " and maximum threshold of " << m;

        world.barrier();
        LOG_SEV_IF(world.rank() == 0, info) << "Time to compute average:              " << dlog::clock_to_string(timer.elapsed());
        timer.restart();
    }

    // Compute and combine persistent integrals
    diy::DiscreteBounds divs {3};
    for (unsigned i = 0; i < 3; ++i)
    {
        divs.min[i] = 0;
        divs.max[i] = decomposer.divisions[i];
    }
    TreeTracer::Boxes boxes(mt_master.size(), divs);
    diy::RegularSwapPartners  partners(decomposer, k, false);    // contiguous = false: distance halving
    diy::reduce(mt_master, assigner, partners, TreeTracer(&boxes, decomposer, pi_master, m, t, avg_fn_list, density_fn, density_weighted));

    world.barrier();
    LOG_SEV_IF(world.rank() == 0, info) << "Time to compute persistent integrals: " << dlog::clock_to_string(timer.elapsed());
    timer.restart();

    // Save persistent integrals to file
    pi_master.foreach(OutputIntegrals(outfn, density_weighted, verbose));

    world.barrier();
    LOG_SEV_IF(world.rank() == 0, info) << "Time to output persistent integrals:  " << dlog::clock_to_string(timer.elapsed());
    timer.restart();

    dlog::prof.flush();     // TODO: this is necessary because the profile file will close before
                            //       the global dlog::prof goes out of scope and flushes the events.
                            //       Need to eventually fix this.
    dlog::stats.flush();
}
