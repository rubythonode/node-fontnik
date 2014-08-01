/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2013 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include <cmath>

// mapnik
#include <mapnik/text/face.hpp>
#include <mapnik/debug.hpp>

// freetype-gl
#include <freetype-gl/distmap.h>

// agg
#include <agg/agg_curves.h>

extern "C"
{
#include FT_GLYPH_H
}

namespace mapnik
{

struct User {
    Rings rings;
    Points ring;
};

font_face::font_face(FT_Face face)
    : face_(face),
      glyphs_(std::make_shared<glyph_cache_type>()),
      char_height_(0.0)
{
}

font_face::font_face(FT_Face face, glyph_cache_ptr glyphs)
    : face_(face),
      glyphs_(glyphs),
      char_height_(0.0) {}

double font_face::get_char_height() const
{
    if (char_height_ != 0.0) return char_height_;
    glyph_info tmp;
    tmp.glyph_index = FT_Get_Char_Index(face_, 'X');
    glyph_dimensions(tmp);
    char_height_ = tmp.height;
    return char_height_;
}

bool font_face::set_character_sizes(double size)
{
    char_height_ = 0.0;
    return !FT_Set_Char_Size(face_,0,(FT_F26Dot6)(size * (1<<6)),0,0);
}

void font_face::glyph_dimensions(glyph_info & glyph) const
{
    //Check if char is already in cache
    auto const& itr = glyphs_->find(glyph.glyph_index);
    if (itr != glyphs_.get()->cend()) {
        glyph = itr->second;
        return;
    }

    // TODO: remove hardcoded buffer size?
    int buffer = 3;

    if (FT_Load_Glyph (face_, glyph.glyph_index, FT_LOAD_NO_HINTING)) return;

    FT_Glyph image;
    if (FT_Get_Glyph(face_->glyph, &image)) return;

    glyph.line_height = face_->size->metrics.height / 64.0;
    glyph.advance = face_->glyph->metrics.horiAdvance / 64.0;
    glyph.ascender = face_->size->metrics.ascender / 64.0;
    glyph.descender = face_->size->metrics.descender / 64.0;

    FT_Glyph_To_Bitmap(&image, FT_RENDER_MODE_NORMAL, 0, 1);

    int width = ((FT_BitmapGlyph)image)->bitmap.width;
    int height = ((FT_BitmapGlyph)image)->bitmap.rows;

    glyph.width = width;
    glyph.height = height;
    glyph.left = ((FT_BitmapGlyph)image)->left;
    glyph.top = ((FT_BitmapGlyph)image)->top;

    // TODO: move this out?
    // Create a signed distance field (SDF) for the glyph bitmap.
    if (width > 0) {
        unsigned int buffered_width = width + 2 * buffer;
        unsigned int buffered_height = height + 2 * buffer;

        unsigned char *distance = make_distance_map((unsigned char *)((FT_BitmapGlyph)image)->bitmap.buffer, width, height, buffer);

        glyph.bitmap.resize(buffered_width * buffered_height);
        for (unsigned int y = 0; y < buffered_height; y++) {
            memcpy((unsigned char *)glyph.bitmap.data() + buffered_width * y, distance + y * distmap_size, buffered_width);
        }
        free(distance);
    }

    FT_Done_Glyph(image);

    glyphs_.get()->emplace(glyph.glyph_index, glyph);
}

void close_ring(Points *ring) {
    Point first = ring->front();
    Point last = ring->back();

    if (first.get<0>() != last.get<0>() || first.get<1>() != last.get<1>()) {
        ring->push_back(first);
    }
}

int move_to(const FT_Vector *to, void *ptr) {
    User *user = (User*)ptr;
    if (!user->ring.empty()) {
        close_ring(&(user->ring));
        user->rings.push_back(user->ring);
        user->ring.clear();
    }
    user->ring.push_back(Point {to->x, to->y});
    return 0;
}

int line_to(const FT_Vector *to, void *ptr) {
    User *user = (User*)ptr;
    user->ring.push_back(Point {to->x, to->y});
    return 0;
}

int conic_to(const FT_Vector *control,
             const FT_Vector *to,
             void *ptr) {
    User *user = (User*)ptr;

    Point prev = user->ring.back();

    // pop off last point, duplicate of first point in bezier curve
    user->ring.pop_back();

    agg::curve3_div curve(prev.get<0>(), prev.get<1>(),
                          control->x, control->y,
                          to->x, to->y);

    curve.rewind(0);
    double x, y;
    unsigned cmd;
    while (agg::path_cmd_stop != (cmd = curve.vertex(&x, &y))) {
        user->ring.push_back(Point {x, y});
    }

    return 0;
}

int cubic_to(const FT_Vector *c1,
             const FT_Vector *c2,
             const FT_Vector *to,
             void *ptr) {
    User *user = (User*)ptr;

    Point prev = user->ring.back();

    // pop off last point, duplicate of first point in bezier curve
    user->ring.pop_back();

    agg::curve4_div curve(prev.get<0>(), prev.get<1>(),
                          c1->x, c1->y,
                          c2->x, c2->y,
                          to->x, to->y);

    curve.rewind(0);
    double x, y;
    unsigned cmd;
    while (agg::path_cmd_stop != (cmd = curve.vertex(&x, &y))) {
        user->ring.push_back(Point {x, y});
    }

    return 0;
}

// point in polygon ray casting algorithm
bool polyContainsPoint(Rings *rings, Point p) {
    bool c = false;

    for (auto ring : *rings) {
        auto p1 = ring.begin();
        auto p2 = ring.begin()++;
        for (; p2 != ring.end(); p1++, p2++) {
            if (((p1->get<1>() > p.get<1>()) != (p2->get<1>() > p.get<1>())) && (p.get<0>() < (p2->get<0>() - p1->get<0>()) * (p.get<1>() - p1->get<1>()) / (p2->get<1>() - p1->get<1>()) + p1->get<0>())) {
                c = !c;
            }
        }
    }

    return c;
}

double squaredDistance(Point *v, Point *w) {
    double a = v->get<0>() - w->get<0>();
    double b = v->get<1>() - w->get<1>();
    return a * a + b * b;
}

Point* projectPointOnLineSegment(Point *p, Point *v, Point *w) {
  double l2 = squaredDistance(v, w);
  if (l2 == 0) return v;
  double t = ((p->get<0>() - v->get<0>()) * (w->get<0>() - v->get<0>()) + (p->get<1>() - v->get<1>()) * (w->get<1>() - v->get<1>())) / l2;
  if (t < 0) return v;
  if (t > 1) return w;

  Point q {
      v->get<0>() + t * (w->get<0>() - v->get<0>()),
      v->get<1>() + t * (w->get<1>() - v->get<1>())
  };

  // TODO: returning ptr to temp is bad, how to fix?
  return &q;
}

double squaredDistanceToLineSegment(Point *p, Point *v, Point *w) {
    Point *s = projectPointOnLineSegment(p, v, w);
    return squaredDistance(p, s);
}

double minDistanceToLineSegment(Tree *tree, Point p, int radius) {
    int squaredRadius = radius * radius;

    std::vector<SegmentValue> results;
    tree->query(bgi::nearest(&p, radius), std::back_inserter(results));

    double squaredDistance = std::numeric_limits<double>::infinity();
    for (auto value : results) {
        SegmentPair segment = value.second;
        double dist = squaredDistanceToLineSegment(&p, &segment.first, &segment.second);
        if (dist < squaredDistance && dist < squaredRadius) {
            squaredDistance = dist;
        }
    }
    return sqrt(squaredDistance);
}

void font_face::glyph_outlines(glyph_info &glyph,
                               int size,
                               int buffer,
                               float cutoff) const
{
    //Check if char is already in cache
    auto const& itr = glyphs_->find(glyph.glyph_index);
    if (itr != glyphs_.get()->cend()) {
        glyph = itr->second;
        return;
    }

    /*
    float scale = face_->units_per_EM / size;
    int ascender = face_->ascender / scale;
    */

    if (FT_Load_Glyph (face_, glyph.glyph_index, FT_LOAD_NO_HINTING)) return;

    FT_Glyph ft_glyph;
    if (FT_Get_Glyph(face_->glyph, &ft_glyph)) return;

    FT_Outline_Funcs func_interface = {
        .move_to = &move_to,
        .line_to = &line_to,
        .conic_to = &conic_to,
        .cubic_to = &cubic_to,
        .shift = 0,
        .delta = 0
    };

    User user;

    // Decompose outline into bezier curves and line segments
    FT_Outline outline = ((FT_OutlineGlyph)ft_glyph)->outline;
    if (FT_Outline_Decompose(&outline, &func_interface, &user)) return;

    if (!user.ring.empty()) {
        close_ring(&(user.ring));
        user.rings.push_back(user.ring);
    }

    if (user.rings.empty()) return;

    // Calculate the real glyph bbox.
    double xMin = std::numeric_limits<double>::infinity(),
           yMin = std::numeric_limits<double>::infinity();

    double xMax = -std::numeric_limits<double>::infinity(),
           yMax = -std::numeric_limits<double>::infinity();

    for (auto ring : user.rings) {
        for (auto point : ring) {
            if (point.get<0>() > xMax) xMax = point.get<0>();
            if (point.get<0>() < xMin) xMin = point.get<0>();
            if (point.get<1>() > yMax) yMax = point.get<1>();
            if (point.get<1>() < yMin) yMin = point.get<1>();
        }
    }

    xMin = round(xMin);
    yMin = round(yMin);
    xMax = round(xMax);
    yMax = round(yMax);

    // Offset so that glyph outlines are in the bounding box.
    for (auto ring : user.rings) {
        for (auto point : ring) {
            point.set<0>(point.get<0>() + -xMin + buffer);
            point.set<1>(point.get<1>() + -yMin + buffer);
        }
    }

    if (xMax - xMin == 0 || yMax - yMin == 0) return;

    glyph.left = xMin;
    glyph.top = -yMin; // -yMin - ascender?
    glyph.width = xMax - xMin;
    glyph.height = yMax - yMin;

    glyph.line_height = face_->size->metrics.height;
    glyph.advance = face_->glyph->metrics.horiAdvance;
    glyph.ascender = face_->size->metrics.ascender;
    glyph.descender = face_->size->metrics.descender;

    Tree tree;
    float offset = 0.5;
    int radius = 8;

    for (auto ring : user.rings) {
        auto next = ring.begin();
        next++;

        for (auto it = ring.begin(); next != ring.end(); it++, next++) {
            int xMin = std::min(it->get<0>(), next->get<0>());
            int xMax = std::max(it->get<0>(), next->get<0>());
            int yMin = std::min(it->get<1>(), next->get<1>());
            int yMax = std::max(it->get<1>(), next->get<1>());

            tree.insert(SegmentValue {
                Box {
                    Point {xMin, yMin},
                    Point {xMax, yMax}
                },
                SegmentPair {
                    Point {it->get<0>(), it->get<1>()},
                    Point {next->get<0>(), next->get<1>()}
                }
            });
        }
    }

    // Loop over every pixel and determine the positive/negative distance to the outline.
    unsigned int buffered_width = glyph.width + 2 * buffer;
    unsigned int buffered_height = glyph.height + 2 * buffer;
    glyph.bitmap.resize(buffered_width * buffered_height);

    for (unsigned int y = 0; y < buffered_height; y++) {
        for (unsigned int x = 0; x < buffered_width; x++) {
            unsigned int i = y * buffered_width + x;

            double d = minDistanceToLineSegment(&tree, Point {x + offset, y + offset}, radius) * (256 / radius);

            // Invert if point is inside.
            bool inside = polyContainsPoint(&user.rings, Point { x + offset, y + offset });
            if (inside) {
                d = -d;
            }

            // Shift the 0 so that we can fit a few negative values
            // into our 8 bits.
            d += cutoff * 256;

            // Clamp to 0-255 to prevent overflows or underflows.
            int n = d > 255 ? 255 : d;
            n = n < 0 ? 0 : n;

            std::string value(1, static_cast<char>(255 - n));
            glyph.bitmap.insert(i, value);
        }
    }

    FT_Done_Glyph(ft_glyph);

    glyphs_.get()->emplace(glyph.glyph_index, glyph);
}

font_face::~font_face()
{
    MAPNIK_LOG_DEBUG(font_face) <<
        "font_face: Clean up face \"" << family_name() <<
        " " << style_name() << "\"";

    FT_Done_Face(face_);
}

/******************************************************************************/

void font_face_set::add(face_ptr face)
{
    faces_.push_back(face);
}

void font_face_set::set_character_sizes(double size)
{
    for (face_ptr const& face : faces_)
    {
        face->set_character_sizes(size);
    }
}

}//ns mapnik
