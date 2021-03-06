#pragma once

#include <glkernel/sample.h>

#include <cassert>
#include <random>
#include <vector>
#include <array>
#include <list>
#include <iterator>
#include <tuple>

#include <glkernel/glm_compatability.h>


namespace glkernel
{

namespace sample
{


// optimization grid for identifying adjacent points

template <typename T, glm::precision P>
struct poisson_square_map
{
    poisson_square_map(const T min_dist)
    : m_none{ static_cast<size_t>(-1) }
    , m_side{ static_cast<size_t>(std::ceil(sqrt(2.0) / min_dist)) }
    , m_dist2(min_dist * min_dist)
    {
        m_mask.resize(m_side * m_side, m_none);
    }

    void mask(const glm::tvec2<T, P> & point, const size_t k)
    {
        const auto s = static_cast<int>(m_side);
        const auto o = static_cast<int>(point.y * s) * s + static_cast<int>(point.x * s);

        assert(m_mask[o] == m_none);

        m_mask[o] = k;
    }

    bool masked(const glm::tvec2<T, P> & probe, const tkernel<glm::tvec2<T, P>> & kernel) const
    {
        const auto s = static_cast<int>(m_side);

        const auto x = static_cast<int>(probe.x * s);
        const auto y = static_cast<int>(probe.y * s);

        const auto corners = std::array<int, 4>{ { y - 2, x - 2, y + 2, x + 2 } };

        for (int j = y - 2; j < y + 3; ++j)
            for (int i = x - 2; i < x + 3; ++i)
            {
                // optimization: skip the 4 corner cases, since the fall not within distance anyway ...
                if ((j == corners[0] || j == corners[2]) && (i == corners[1] || i == corners[3])) 
                    continue;

                const auto i_tiled = i < 0 ? i + s : i % s;
                const auto j_tiled = j < 0 ? j + s : j % s;

                const auto o = m_mask[j_tiled * s + i_tiled];
                if (o == m_none)
                    continue;

                auto masking_probe = kernel[o];

                if (i < 0)
                    masking_probe.x -= 1.0;
                else if (i >= s)
                    masking_probe.x += 1.0;

                if (j < 0)
                    masking_probe.y -= 1.0;
                else if (j >= s)
                    masking_probe.y += 1.0;

                // also optimized by using square distance->skipping sqrt
                const auto delta = masking_probe - probe;
                if (glm::dot(delta, delta) < m_dist2)
                    return true;
            }

        return false;
    }

protected:
    size_t m_none;

    size_t m_side;
    T m_dist2;

    std::vector<size_t> m_mask;
};


template <typename T, glm::precision P>
size_t poisson_square(tkernel<glm::tvec2<T, P>> & kernel, const unsigned int num_probes)
{
    assert(kernel.depth() == 1);

    const T min_dist = 1 / sqrt(static_cast<T>(kernel.size() * sqrt(2)));
    return poisson_square(kernel, min_dist, num_probes);
}


template <typename T, glm::precision P>
size_t poisson_square(tkernel<glm::tvec2<T, P>> & kernel, const T min_dist, const unsigned int num_probes)
{
    assert(kernel.depth() == 1);

    std::random_device RD;
    std::mt19937_64 generator(RD());

    std::uniform_real_distribution<> radius_dist(min_dist, min_dist * 2.0);
    std::uniform_real_distribution<> angle_dist(0.0, 2.0 * glm::pi<T>());

    std::uniform_int_distribution<> int_distribute(0, std::numeric_limits<int>::max());

    auto occupancy = poisson_square_map<T, P>{ min_dist };

    size_t k = 0; // number of valid/final points within the kernel
    kernel[k] = glm::tvec2<T, P>(0.5, 0.5);

    auto actives = std::list<size_t>();
    actives.push_back(k);

    occupancy.mask(kernel[k], k);

    while (!actives.empty() && k < kernel.size() - 1)
    {
        // randomly pick an active point
        const auto pick = int_distribute(generator);

        auto pick_it = actives.begin();
        std::advance(pick_it, pick % actives.size());

        const auto active = kernel[*pick_it];


        std::vector<std::tuple<glm::tvec2<T, P>, T>> probes{ num_probes };

        #pragma omp parallel for
        for (int i = 0; i < static_cast<int>(num_probes); ++i)
        {
            const auto r = radius_dist(generator);
            const auto a = angle_dist(generator);

            auto probe = glm::tvec2<T, P>{ active.x + r * cos(a), active.y + r * sin(a) };

            // within square? (tilable)
            if (probe.x < 0.0)
                probe.x += 1.0;
            else if (probe.x >= 1.0)
                probe.x -= 1.0;

            if (probe.y < 0.0)
                probe.y += 1.0;
            else if (probe.y >= 1.0)
                probe.y -= 1.0;

            // Note: do NOT make this optimization
            //if (!tilable && (probe.x < 0.0 || probe.x > 1.0 || probe.y < 0.0 || probe.y > 1.0))
            //    continue;

            // points within min_dist?
            const auto masked = occupancy.masked(probe, kernel);
            const auto delta = glm::abs(active - probe);

            probes[i] = std::make_tuple<glm::tvec2<T, P>, T>(std::move(probe), (masked ? static_cast<T>(-1.0) : glm::dot(delta, delta)));
        }
        
        // pick nearest probe from sample set
        glm::vec2 nearest_probe;
        auto nearest_dist = 4 * min_dist * min_dist;
        auto nearest_found = false;

        for (int i = 0; i < static_cast<int>(num_probes); ++i)
        {
            // is this nearest point yet? - optimized by using square distance -> skipping sqrt
            const auto new_dist = std::get<1>(probes[i]);
            if (new_dist < 0.0 || nearest_dist < new_dist)
                continue;

            if (!nearest_found)
                nearest_found = true;

            nearest_dist = new_dist;
            nearest_probe = std::get<0>(probes[i]);
        }

        if (!nearest_found && (actives.size() > 0 || k > 1))
        {
            actives.erase(pick_it);
            continue;
        }

        kernel[++k] = nearest_probe;
        actives.push_back(k);

        occupancy.mask(nearest_probe, k);
    }

    return k + 1;
}


} // namespace sample

} // namespace glkernel
