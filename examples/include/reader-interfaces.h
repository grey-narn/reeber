#ifndef REEBER_READER_INTERFACES_H
#define REEBER_READER_INTERFACES_H

#include <boost/algorithm/string/predicate.hpp>

#include <diy/io/numpy.hpp>

#ifdef REEBER_USE_BOXLIB_READER
#include <algorithm>
#include <reeber/io/boxlib.h>
namespace r = reeber;
#endif

#include "reeber-real.h"

struct Reader
{
    typedef                 std::vector<int>                            Shape;
    typedef                 std::vector<Real>                           Size;
    virtual const Shape&    shape() const                               =0;
    virtual const Size&     cell_size() const                           =0;
    virtual void            read(const diy::DiscreteBounds& bounds,     // Region to read
                                 Real* buffer,                          // Buffer where data will be copied to
                                 bool collective = true) const          =0;
    virtual                 ~Reader()                                   {}
    static Reader*          create(std::string, diy::mpi::communicator);
};

struct NumPyReader: public Reader
{
                            NumPyReader(std::string             infn,
                                        diy::mpi::communicator  world):
                                in(world, infn, diy::mpi::io::file::rdonly),
                                numpy_reader(in), dx(3, 1.0)        { numpy_reader.read_header(); }

    virtual const Shape&    shape() const                           { return numpy_reader.shape(); }
    virtual const Size&     cell_size() const                       { return dx; }

    virtual void            read(const diy::DiscreteBounds& bounds,
                                 Real* buffer,
                                 bool collective = true) const      { numpy_reader.read(bounds, buffer, collective); }

    diy::mpi::io::file      in;
    diy::io::NumPy          numpy_reader;
    Size                    dx;
};

#ifdef REEBER_USE_BOXLIB_READER
struct BoxLibReader: public Reader
{
                            BoxLibReader(std::string            infn,
                                         diy::mpi::communicator world):
                                boxlib_reader(infn, world)          {}


    virtual const Shape&    shape() const                           { return boxlib_reader.shape(); }
    virtual const Size&     cell_size() const                       { return boxlib_reader.cell_size(); }
    virtual void            read(const diy::DiscreteBounds& bounds,
                                 Real* buffer,
                                 bool collective = true) const      { boxlib_reader.read(bounds, buffer, collective); }

    r::io::BoxLib::Reader   boxlib_reader;
};

struct BoxLibInSituCopier: public Reader
{
                               BoxLibInSituCopier(const MultiFab&        simulation_data,
                                                  const Geometry&        geometry,
                                                  int                    component,
                                                  diy::mpi::communicator world):
                                   boxlib_copier(simulation_data, geometry,
                                                 component, world)     {}


    virtual const Shape&       shape() const                           { return boxlib_copier.shape(); }
    virtual const Size&        cell_size() const                       { return boxlib_copier.cell_size(); }
    virtual void               read(const diy::DiscreteBounds& bounds,
                                     Real* buffer,
                                     bool collective = true) const     { boxlib_copier.read(bounds, buffer, collective); }

    r::io::BoxLib::InSituCopier boxlib_copier;
};

#endif

inline Reader* Reader::create(std::string infn, diy::mpi::communicator world)
{
    Reader* reader_ptr;
#ifdef REEBER_USE_BOXLIB_READER
    if (boost::algorithm::ends_with(infn, ".npy"))
        reader_ptr = new NumPyReader(infn, world);
    else
        reader_ptr = new BoxLibReader(infn, world);
#else
    reader_ptr      = new NumPyReader(infn, world);
#endif
    return reader_ptr;
}

#endif
