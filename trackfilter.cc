/*

    Track manipulation filter
    Copyright (c) 2009 - 2013 Robert Lipe, robertlipe+source@gpsbabel.org
    Copyright (C) 2005-2006 Olaf Klein, o.b.klein@gpsbabel.org

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111 USA

 */

#undef TRACKF_DBG

#include "defs.h"
#include "filterdefs.h"
#include "queue.h"                         // for queue, QUEUE_FOR_EACH, QUEUE_FIRST, QUEUE_LAST, QUEUE_NEXT
#include "grtcirc.h"                       // for RAD, gcdist, heading_true_degrees, radtometers, radtomiles
#include "strptime.h"
#include "trackfilter.h"
#include "src/core/datetime.h"             // for DateTime
#include <QtCore/QByteArray>               // for QByteArray
#include <QtCore/QChar>                    // for QChar
#include <QtCore/QDate>                    // for QDate
#include <QtCore/QDateTime>                // for QDateTime
#ifdef TRACKF_DBG
#include <QtCore/QDebug>
#endif
#include <QtCore/QtGlobal>                 // for qint64, qPrintable
#include <QtCore/QRegExp>                  // for QRegExp, QRegExp::WildcardUnix
#include <QtCore/QRegularExpression>       // for QRegularExpression
#include <QtCore/QRegularExpressionMatch>  // for QRegularExpressionMatch
#include <QtCore/QString>                  // for QString
#include <QtCore/Qt>                       // for UTC, CaseInsensitive
#include <cassert>                         // for assert
#include <cstdio>                          // for printf
#include <cstdlib>                         // for abs, atoi, qsort
#include <cstring>                         // for strlen, strchr, strcmp
#include <ctype.h>                         // for tolower, isdigit
#include <cmath>                           // for nan
#include <ctime>                           // for gmtime, strftime

#if FILTERS_ENABLED || MINIMAL_FILTERS
#define MYNAME "trackfilter"

/*******************************************************************************
* helpers
*******************************************************************************/

int TrackFilter::trackfilter_opt_count()
{
  int res = 0;
  arglist_t* a = args;

  while (a->argstring) {
    if (*a->argval != nullptr) {
      res++;
    }
    a++;
  }
  return res;
}

qint64 TrackFilter::trackfilter_parse_time_opt(const char* arg)
{
  qint64 result;

  QRegularExpression re("^([+-]?\\d+)([dhms])$", QRegularExpression::CaseInsensitiveOption);
  assert(re.isValid());
  QRegularExpressionMatch match = re.match(arg);
  if (match.hasMatch()) {
    bool ok;
    result = match.captured(1).toLong(&ok);
    if (!ok) {
      fatal(MYNAME "-time: invalid quantity in move option \"%s\"!\n", qPrintable(match.captured(1)));
    }

    switch (match.captured(2).at(0).toLower().toLatin1()) {
    case 'd':
      result *= SECONDS_PER_DAY;
      break;
    case 'h':
      result *= SECONDS_PER_HOUR;
      break;
    case 'm':
      result *= 60;
      break;
    case 's':
      break;
    default:
      fatal(MYNAME "-time: invalid unit in move option \"%s\"!\n", qPrintable(match.captured(2)));
    }

#ifdef TRACKF_DBG
    qDebug() << MYNAME "-time option: shift =" << result << "seconds";
#endif
  } else {
    fatal(MYNAME "-time: invalid value in move option \"%s\"!\n", arg);
  }

  return result;
}

int TrackFilter::trackfilter_init_qsort_cb(const void* a, const void* b)
{
  const trkflt_t* ra = (const trkflt_t*) a;
  const trkflt_t* rb = (const trkflt_t*) b;
  const QDateTime dta = ra->first_time;
  const QDateTime dtb = rb->first_time;

  if (dta > dtb) {
    return 1;
  }
  if (dta == dtb) {
    return 0;
  }
  return -1;
}

int TrackFilter::trackfilter_merge_qsort_cb(const void* a, const void* b)
{
  const Waypoint* wa = *(Waypoint**)a;
  const Waypoint* wb = *(Waypoint**)b;
  const QDateTime dta = wa->GetCreationTime();
  const QDateTime dtb = wb->GetCreationTime();

  if (dta > dtb) {
    return 1;
  }
  if (dta == dtb) {
    int seqno_a = gb_ptr2int(wa->extra_data);
    int seqno_b = gb_ptr2int(wb->extra_data);
    if (seqno_a > seqno_b) {
      return 1;
    } else if (seqno_a == seqno_b) {
      return 0;
    } else {
      return -1;
    }
  }
  return -1;
}

fix_type TrackFilter::trackfilter_parse_fix(int* nsats)
{
  if (!opt_fix) {
    return fix_unknown;
  }
  if (!case_ignore_strcmp(opt_fix, "pps")) {
    *nsats = 4;
    return fix_pps;
  }
  if (!case_ignore_strcmp(opt_fix, "dgps")) {
    *nsats = 4;
    return fix_dgps;
  }
  if (!case_ignore_strcmp(opt_fix, "3d")) {
    *nsats = 4;
    return fix_3d;
  }
  if (!case_ignore_strcmp(opt_fix, "2d")) {
    *nsats = 3;
    return fix_2d;
  }
  if (!case_ignore_strcmp(opt_fix, "none")) {
    *nsats = 0;
    return fix_none;
  }
  fatal(MYNAME ": invalid fix type\n");
  return fix_unknown;
}

void TrackFilter::trackfilter_fill_track_list_cb(const route_head* track) 	/* callback for track_disp_all */
{
  queue* elem, *tmp;

  if (track->rte_waypt_ct == 0) {
    track_del_head(const_cast<route_head*>(track));
    return;
  }

  if (opt_name != nullptr) {
    if (!QRegExp(opt_name, Qt::CaseInsensitive, QRegExp::WildcardUnix).exactMatch(track->rte_name)) {
      QUEUE_FOR_EACH(&track->waypoint_list, elem, tmp) {
        auto wpt = reinterpret_cast<Waypoint*>(elem);
        track_del_wpt(const_cast<route_head*>(track), wpt);
        delete wpt;
      }
      track_del_head(const_cast<route_head*>(track));
      return;
    }
  }

  track_list[track_ct].track = const_cast<route_head*>(track);

  int i = 0;
  Waypoint* prev = nullptr;

  QUEUE_FOR_EACH(&track->waypoint_list, elem, tmp) {
    track_pts++;

    auto wpt = reinterpret_cast<Waypoint*>(elem);
    if (!wpt->creation_time.isValid()) {
      timeless_pts++;
    }
    if (!(opt_merge && opt_discard) && need_time && (!wpt->creation_time.isValid())) {
      fatal(MYNAME "-init: Found track point at %f,%f without time!\n",
            wpt->latitude, wpt->longitude);
    }

    i++;
    if (i == 1) {
      track_list[track_ct].first_time = wpt->GetCreationTime();
    } else if (i == track->rte_waypt_ct) {
      track_list[track_ct].last_time = wpt->GetCreationTime();
    }

    if (need_time && (prev != nullptr) && (prev->GetCreationTime() > wpt->GetCreationTime())) {
      if (opt_merge == nullptr) {
        QString t1 = prev->CreationTimeXML();
        QString t2 = wpt->CreationTimeXML();
        fatal(MYNAME "-init: Track points badly ordered (timestamp %s > %s)!\n", qPrintable(t1), qPrintable(t2));
      }
    }
    prev = wpt;
  }
  track_ct++;
}

void TrackFilter::trackfilter_minpoint_list_cb(const route_head* track)
{
  int minimum_points = atoi(opt_minpoints);
  if (track->rte_waypt_ct < minimum_points) {
    track_del_head(const_cast<route_head*>(track));
    return;
  }
}

/*******************************************************************************
* track title producers
*******************************************************************************/

void TrackFilter::trackfilter_split_init_rte_name(route_head* track, const QDateTime& dt)
{
  QString datetimestring;

  if (opt_interval != 0) {
    datetimestring = dt.toUTC().toString("yyyyMMddhhmmss");
  } else {
    datetimestring = dt.toUTC().toString("yyyyMMdd");
  }

  if ((opt_title != nullptr) && (strlen(opt_title) > 0)) {
    if (strchr(opt_title, '%') != nullptr) {
      // Uggh.  strftime format exposed to user.

      time_t time = dt.toTime_t();
      struct tm tm = *gmtime(&time);
      char buff[128];
      strftime(buff, sizeof(buff), opt_title, &tm);
      track->rte_name = buff;
    } else {
      track->rte_name = QString("%1-%2").arg(opt_title, datetimestring);
    }
  } else if (!track->rte_name.isEmpty()) {
    track->rte_name = QString("%1-%2").arg(track->rte_name, datetimestring);
  } else {
    track->rte_name = datetimestring;
  }
}

void TrackFilter::trackfilter_pack_init_rte_name(route_head* track, const QDateTime& default_time)
{
  if (strchr(opt_title, '%') != nullptr) {
    // Uggh.  strftime format exposed to user.

    QDateTime dt;
    if (track->rte_waypt_ct == 0) {
      dt = default_time;
    } else {
      auto wpt = reinterpret_cast<Waypoint*>QUEUE_FIRST(&track->waypoint_list);
      dt = wpt->GetCreationTime();
    }
    time_t t = dt.toTime_t();
    struct tm tm = *gmtime(&t);
    char buff[128];
    strftime(buff, sizeof(buff), opt_title, &tm);
    track->rte_name = buff;
  } else {
    track->rte_name = opt_title;
  }
}

/*******************************************************************************
* option "title"
*******************************************************************************/

void TrackFilter::trackfilter_title()
{
  if (opt_title == nullptr) {
    return;
  }

  if (strlen(opt_title) == 0) {
    fatal(MYNAME "-title: Missing your title!\n");
  }
  for (int i = 0; i < track_ct; i++) {
    route_head* track = track_list[i].track;
    trackfilter_pack_init_rte_name(track, QDateTime::fromMSecsSinceEpoch(0, Qt::UTC));
  }
}

/*******************************************************************************
* option "pack" (default)
*******************************************************************************/

void TrackFilter::trackfilter_pack()
{
  int i, j;

  for (i = 1, j = 0; i < track_ct; i++, j++) {
    trkflt_t prev = track_list[j];
    if (prev.last_time >= track_list[i].first_time) {
      fatal(MYNAME "-pack: Tracks overlap in time! %s >= %s at %d\n",
            qPrintable(prev.last_time.toString()),
            qPrintable(track_list[i].first_time.toString()), i);
    }
  }

  /* we fill up the first track by all other track points */

  route_head* master = track_list[0].track;

  for (i = 1; i < track_ct; i++) {
    queue* elem, *tmp;
    route_head* curr = track_list[i].track;

    QUEUE_FOR_EACH(&curr->waypoint_list, elem, tmp) {
      auto wpt = reinterpret_cast<Waypoint*>(elem);
      track_del_wpt(curr, wpt);
      track_add_wpt(master, wpt);
    }
    track_del_head(curr);
    track_list[i].track = nullptr;
  }
  track_ct = 1;
}

/*******************************************************************************
* option "merge"
*******************************************************************************/

void TrackFilter::trackfilter_merge()
{
  int i;

  queue* elem, *tmp;
  Waypoint* wpt;
  route_head* master = track_list[0].track;

  if (track_pts-timeless_pts < 1) {
    return;
  }

  Waypoint** buff = (Waypoint**)xcalloc(track_pts-timeless_pts, sizeof(*buff));

  int j = 0;
  for (i = 0; i < track_ct; i++) {	/* put all points into temp buffer */
    route_head* track = track_list[i].track;
    QUEUE_FOR_EACH(&track->waypoint_list, elem, tmp) {
      wpt = reinterpret_cast<Waypoint*>(elem);
      if (wpt->creation_time.isValid()) {
        buff[j] = new Waypoint(*wpt);
        // augment sort key so a stable sort is possible.
        buff[j]->extra_data = gb_int2ptr(j);
        j++;
        // we will put the merged points in one track segment,
        // as it isn't clear how track segments in the original tracks
        // should relate to the merged track.
        // track_add_wpt will set new_trkseg for the first point
        // after the sort.
        wpt->wpt_flags.new_trkseg = 0;
      }
      track_del_wpt(track, wpt); // copies any new_trkseg flag forward.
      delete wpt;
    }
    if (track != master) {	/* i > 0 */
      track_del_head(track);
    }
  }
  track_ct = 1;

  qsort(buff, track_pts-timeless_pts, sizeof(*buff), trackfilter_merge_qsort_cb);

  int dropped = timeless_pts;
  Waypoint* prev = nullptr;

  for (i = 0; i < track_pts-timeless_pts; i++) {
    buff[i]->extra_data = nullptr;
    wpt = buff[i];
    if ((prev == nullptr) || (prev->GetCreationTime() != wpt->GetCreationTime())) {
      track_add_wpt(master, wpt);
      prev = wpt;
    } else {
      delete wpt;
      dropped++;
    }
  }
  xfree(buff);

  if (global_opts.verbose_status > 0) {
    printf(MYNAME "-merge: %d track point(s) merged, %d dropped.\n", track_pts - dropped, dropped);
  }
}

/*******************************************************************************
* option "split"
*******************************************************************************/

void TrackFilter::trackfilter_split()
{
  route_head* master = track_list[0].track;
  int count = master->rte_waypt_ct;

  Waypoint* wpt;
  queue* elem, *tmp;
  int i, j;
  double interval = -1; /* seconds */
  double distance = -1; /* meters */

  if (count <= 1) {
    return;
  }

  /* check additional options */

  opt_interval = (opt_split && (strlen(opt_split) > 0) && (0 != strcmp(opt_split, TRACKFILTER_SPLIT_OPTION)));
  if (opt_interval != 0) {
    QRegularExpression re("^([+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+))([dhms])$", QRegularExpression::CaseInsensitiveOption);
    assert(re.isValid());
    QRegularExpressionMatch match = re.match(opt_split);
    if (match.hasMatch()) {
      bool ok;
      interval = match.captured(1).toDouble(&ok);
      if (!ok || interval <= 0.0) {
        fatal(MYNAME ": invalid time interval specified \"%s\", must be a positive number.\n", qPrintable(match.captured(1)));
      }

      switch (match.captured(2).at(0).toLower().toLatin1()) {
      case 'd':
        interval *= SECONDS_PER_DAY;
        break;
      case 'h':
        interval *= SECONDS_PER_HOUR;
        break;
      case 'm':
        interval *= 60;
        break;
      case 's':
        break;
      default:
        fatal(MYNAME ": invalid time interval unit specified.\n");
      }

#ifdef TRACKF_DBG
      printf(MYNAME ": interval %f seconds\n", interval);
#endif
    } else {
      fatal(MYNAME ": invalid timer interval specified \"%s\", must be a positive number, followed by 'd' for days, 'h' for hours, 'm' for minutes or 's' for seconds.\n", opt_split);
    }
  }

  opt_distance = (opt_sdistance && (strlen(opt_sdistance) > 0) && (0 != strcmp(opt_sdistance, TRACKFILTER_SDIST_OPTION)));
  if (opt_distance != 0) {
    QRegularExpression re("^([+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+))([km])$", QRegularExpression::CaseInsensitiveOption);
    assert(re.isValid());
    QRegularExpressionMatch match = re.match(opt_sdistance);
    if (match.hasMatch()) {
      bool ok;
      distance = match.captured(1).toDouble(&ok);
      if (!ok || distance <= 0.0) {
        fatal(MYNAME ": invalid time distance specified \"%s\", must be a positive number.\n", qPrintable(match.captured(1)));
      }

      switch (match.captured(2).at(0).toLower().toLatin1()) {
      case 'k': /* kilometers */
        distance *= 1000.0;
        break;
      case 'm': /* miles */
        distance *= 1609.344;
        break;
      default:
        fatal(MYNAME ": invalid distance unit specified.\n");
      }

#ifdef TRACKF_DBG
      printf(MYNAME ": distance %f meters\n", distance);
#endif
    } else {
      fatal(MYNAME ": invalid distance specified \"%s\", must be a positive number followed by 'k' for kilometers or 'm' for miles.\n", opt_sdistance);
    }
  }

  trackfilter_split_init_rte_name(master, track_list[0].first_time);

  Waypoint** buff = (Waypoint**) xcalloc(count, sizeof(*buff));

  i = 0;
  QUEUE_FOR_EACH(&master->waypoint_list, elem, tmp) {
    wpt = reinterpret_cast<Waypoint*>(elem);
    buff[i++] = wpt;
  }

  route_head* curr = nullptr;	/* will be set by first new track */

  for (i=0, j=1; j<count; i++, j++) {
    bool new_track_flag;

    if ((opt_interval == 0) && (opt_distance == 0)) {
// FIXME: This whole function needs to be reconsidered for arbitrary time.
      new_track_flag = buff[i]->GetCreationTime().toLocalTime().date() !=
                       buff[j]->GetCreationTime().toLocalTime().date();
#ifdef TRACKF_DBG
      if (new_track_flag) {
        printf(MYNAME ": new day %s\n", qPrintable(buff[j]->GetCreationTime().toLocalTime().date().toString(Qt::ISODate)));
      }
#endif
    } else {
      new_track_flag = true;

      if (distance > 0) {
        double rt1 = RAD(buff[i]->latitude);
        double rn1 = RAD(buff[i]->longitude);
        double rt2 = RAD(buff[j]->latitude);
        double rn2 = RAD(buff[j]->longitude);
        double curdist = gcdist(rt1, rn1, rt2, rn2);
        curdist = radtometers(curdist);
        if (curdist <= distance) {
          new_track_flag = false;
        }
#ifdef TRACKF_DBG
        else {
          printf(MYNAME ": sdistance, %g > %g\n", curdist, distance);
        }
#endif
      }

      if (interval > 0) {
        double tr_interval = 0.001 * buff[i]->GetCreationTime().msecsTo(buff[j]->GetCreationTime());
        if (tr_interval <= interval) {
          new_track_flag = false;
        }
#ifdef TRACKF_DBG
        else {
          printf(MYNAME ": split, %g > %g\n", tr_interval, interval);
        }
#endif
      }

    }
    if (new_track_flag) {
#ifdef TRACKF_DBG
      printf(MYNAME ": splitting new track\n");
#endif
      curr = route_head_alloc();
      trackfilter_split_init_rte_name(curr, buff[j]->GetCreationTime());
      track_add_head(curr);
    }
    if (curr != nullptr) {
      wpt = buff[j];
      track_del_wpt(master, wpt);
      track_add_wpt(curr, wpt);
      buff[j] = wpt;
    }
  }
  xfree(buff);
}

/*******************************************************************************
* option "move"
*******************************************************************************/

void TrackFilter::trackfilter_move()
{
  queue* elem, *tmp;

  qint64 delta = trackfilter_parse_time_opt(opt_move);
  if (delta == 0) {
    return;
  }

  for (int i = 0; i < track_ct; i++) {
    route_head* track = track_list[i].track;
    QUEUE_FOR_EACH(&track->waypoint_list, elem, tmp) {
      auto wpt = reinterpret_cast<Waypoint*>(elem);
      wpt->creation_time = wpt->creation_time.addSecs(delta);
    }

    track_list[i].first_time = track_list[i].first_time.addSecs(delta);
    track_list[i].last_time = track_list[i].last_time.addSecs(delta);
  }
}

/*******************************************************************************
* options "fix", "course", "speed"
*******************************************************************************/

void TrackFilter::trackfilter_synth()
{
  queue* elem, *tmp;

  double last_course_lat;
  double last_course_lon;
  double last_speed_lat = std::nan(""); /* Quiet gcc 7.3.0 -Wmaybe-uninitialized */
  double last_speed_lon = std::nan(""); /* Quiet gcc 7.3.0 -Wmaybe-uninitialized */
  gpsbabel::DateTime last_speed_time;
  int nsats = 0;

  fix_type fix = trackfilter_parse_fix(&nsats);

  for (int i = 0; i < track_ct; i++) {
    route_head* track = track_list[i].track;
    bool first = true;
    QUEUE_FOR_EACH(&track->waypoint_list, elem, tmp) {
      auto wpt = reinterpret_cast<Waypoint*>(elem);
      if (opt_fix) {
        wpt->fix = fix;
        if (wpt->sat == 0) {
          wpt->sat = nsats;
        }
      }
      if (first) {
        if (opt_course) {
          // TODO: the course value 0 isn't valid, wouldn't it be better to UNSET course?
          WAYPT_SET(wpt, course, 0);
        }
        if (opt_speed) {
          // TODO: the speed value 0 isn't valid, wouldn't it be better to UNSET speed?
          WAYPT_SET(wpt, speed, 0);
        }
        first = false;
        last_course_lat = wpt->latitude;
        last_course_lon = wpt->longitude;
        last_speed_lat = wpt->latitude;
        last_speed_lon = wpt->longitude;
        last_speed_time = wpt->GetCreationTime();
      } else {
        if (opt_course) {
          WAYPT_SET(wpt, course, heading_true_degrees(RAD(last_course_lat),
                    RAD(last_course_lon),RAD(wpt->latitude),
                    RAD(wpt->longitude)));
          last_course_lat = wpt->latitude;
          last_course_lon = wpt->longitude;
        }
        if (opt_speed) {
          if (last_speed_time.msecsTo(wpt->GetCreationTime()) != 0) {
            // If we have mutliple points with the same time and
            // we use the pair of points about which the time ticks then we will
            // underestimate the distance and compute low speeds on average.
            // Therefore, if we have multiple points with the same time use the
            // first ones with the new times to compute speed.
            // Note that points with the same time can occur because the input
            // has truncated times, or because we are truncating times with
            // toTime_t().
            WAYPT_SET(wpt, speed, radtometers(gcdist(
                                                RAD(last_speed_lat), RAD(last_speed_lon),
                                                RAD(wpt->latitude),
                                                RAD(wpt->longitude))) /
                      (0.001 * std::abs(last_speed_time.msecsTo(wpt->GetCreationTime())))
                     );
            last_speed_lat = wpt->latitude;
            last_speed_lon = wpt->longitude;
            last_speed_time = wpt->GetCreationTime();
          } else {
            WAYPT_UNSET(wpt, speed);
          }
        }
      }
    }
  }
}


/*******************************************************************************
* option: "start" / "stop"
*******************************************************************************/

QDateTime TrackFilter::trackfilter_range_check(const char* timestr)
{
  QDateTime result;

  QRegularExpression re("^(\\d{0,14})$");
  assert(re.isValid());
  QRegularExpressionMatch match = re.match(timestr);
  if (match.hasMatch()) {
    QString start = match.captured(1);
    QString fmtstart("00000101000000");
    fmtstart.replace(0, start.size(), start);
    result = QDateTime::fromString(fmtstart, "yyyyMMddHHmmss");
    result.setTimeSpec(Qt::UTC);
    if (!result.isValid()) {
      fatal(MYNAME "-range-check: Invalid timestamp \"%s\"!\n", qPrintable(start));
    }

#ifdef TRACKF_DBG
    qDebug() << MYNAME "-range-check: " << result;
#endif
  } else {
    fatal(MYNAME "-range-check: Invalid value for option \"%s\"!\n", timestr);
  }

  return result;
}

int TrackFilter::trackfilter_range()		/* returns number of track points left after filtering */
{
  QDateTime start, stop; // constructed such that isValid() is false, unlike gpsbabel::DateTime!
  queue* elem, *tmp;

  if (opt_start != nullptr) {
    start = trackfilter_range_check(opt_start);
  }

  if (opt_stop != nullptr) {
    stop = trackfilter_range_check(opt_stop);
  }

  int dropped = 0;

  for (int i = 0; i < track_ct; i++) {
    route_head* track = track_list[i].track;

    QUEUE_FOR_EACH(&track->waypoint_list, elem, tmp) {
      auto wpt = reinterpret_cast<Waypoint*>(elem);
      bool inside;
      if (wpt->creation_time.isValid()) {
        bool after_start = !start.isValid() || (wpt->GetCreationTime() >= start);
        bool before_stop = !stop.isValid() || (wpt->GetCreationTime() <= stop);
        inside = after_start && before_stop;
      } else {
        // If the time is mangled so horribly that it's
        // negative, toss it.
        inside = false;
      }

      if (!inside) {
        track_del_wpt(track, wpt);
        delete wpt;
        dropped++;
      }
    }

    if (track->rte_waypt_ct == 0) {
      track_del_head(track);
      track_list[i].track = nullptr;
    }
  }

  if ((track_pts > 0) && (dropped == track_pts)) {
    warning(MYNAME "-range: All %d track points have been dropped!\n", track_pts);
  }

  return track_pts - dropped;
}

/*******************************************************************************
* option "seg2trk"
*******************************************************************************/

void TrackFilter::trackfilter_seg2trk()
{
  for (int i = 0; i < track_ct; i++) {
    queue* elem, *tmp;
    route_head* src = track_list[i].track;
    route_head* dest = nullptr;
    route_head* insert_point = src;
    int trk_seg_num = 1;
    bool first = true;

    QUEUE_FOR_EACH(&src->waypoint_list, elem, tmp) {
      auto wpt = reinterpret_cast<Waypoint*>(elem);
      if (wpt->wpt_flags.new_trkseg && !first) {

        dest = route_head_alloc();
        dest->rte_num = src->rte_num;
        /* name in the form TRACKNAME #n */
        if (!src->rte_name.isEmpty()) {
          dest->rte_name = QString("%1 #%2").arg(src->rte_name).arg(++trk_seg_num);
        }

        /* Insert after original track or after last newly
         * created track */
        track_insert_head(dest, insert_point);
        insert_point = dest;
      }

      /* If we found a track separator, transfer from original to
       * new track. We have to reset new_trkseg temporarily to
       * prevent track_del_wpt() from copying it to the next track
       * point.
       */
      if (dest) {
        unsigned orig_new_trkseg = wpt->wpt_flags.new_trkseg;
        wpt->wpt_flags.new_trkseg = 0;
        track_del_wpt(src, wpt);
        wpt->wpt_flags.new_trkseg = orig_new_trkseg;
        track_add_wpt(dest, wpt);
      }
      first = false;
    }
  }
}

/*******************************************************************************
* option "trk2seg"
*******************************************************************************/

void TrackFilter::trackfilter_trk2seg()
{
  route_head* master = track_list[0].track;

  for (int i = 1; i < track_ct; i++) {
    queue* elem, *tmp;
    route_head* curr = track_list[i].track;

    bool first = true;
    QUEUE_FOR_EACH(&curr->waypoint_list, elem, tmp) {
      auto wpt = reinterpret_cast<Waypoint*>(elem);

      unsigned orig_new_trkseg = wpt->wpt_flags.new_trkseg;
      wpt->wpt_flags.new_trkseg = 0;
      track_del_wpt(curr, wpt);
      wpt->wpt_flags.new_trkseg = orig_new_trkseg;
      track_add_wpt(master, wpt);
      if (first) {
        wpt->wpt_flags.new_trkseg = 1;
        first = false;
      }
    }
    track_del_head(curr);
    track_list[i].track = nullptr;
  }
  track_ct = 1;
}

/*******************************************************************************
* option: "faketime"
*******************************************************************************/

TrackFilter::faketime_t TrackFilter::trackfilter_faketime_check(const char* timestr)
{
  faketime_t result;

  QRegularExpression re("^(f?)(\\d{0,14})(?:\\+(\\d{1,10}))?$");
  assert(re.isValid());
  QRegularExpressionMatch match = re.match(timestr);
  if (match.hasMatch()) {
    result.force = match.capturedLength(1) > 0;

    QString start = match.captured(2);
    QString fmtstart("00000101000000");
    fmtstart.replace(0, start.size(), start);
    result.start = QDateTime::fromString(fmtstart, "yyyyMMddHHmmss");
    result.start.setTimeSpec(Qt::UTC);
    if (!result.start.isValid()) {
      fatal(MYNAME "-faketime-check: Invalid timestamp \"%s\"!\n", qPrintable(start));
    }

    if (match.capturedLength(3) > 0) {
      bool ok;
      result.step = match.captured(3).toInt(&ok);
      if (!ok) {
        fatal(MYNAME "-faketime-check: Invalid step \"%s\"!\n", qPrintable(match.captured(3)));
      }
    } else {
      result.step = 0;
    }

#ifdef TRACKF_DBG
    qDebug() << MYNAME "-faketime option: force =" << result.force << ", timestamp =" << result.start << ", step =" << result.step;
#endif
  } else {
    fatal(MYNAME "-faketime-check: Invalid value for faketime option \"%s\"!\n", timestr);
  }

  return result;
}

void TrackFilter::trackfilter_faketime()
{
  queue* elem, *tmp;

  assert(opt_faketime != nullptr);
  faketime_t faketime = trackfilter_faketime_check(opt_faketime);

  for (int i = 0; i < track_ct; i++) {
    route_head* track = track_list[i].track;

    QUEUE_FOR_EACH(&track->waypoint_list, elem, tmp) {
      auto wpt = reinterpret_cast<Waypoint*>(elem);

      if (!wpt->creation_time.isValid() || faketime.force) {
        wpt->creation_time = faketime.start;
        faketime.start = faketime.start.addSecs(faketime.step);
      }
    }
  }
}

bool TrackFilter::trackfilter_points_are_same(const Waypoint* wpta, const Waypoint* wptb)
{
  // We use a simpler (non great circle) test for lat/lon here as this
  // is used for keeping the 'bookends' of non-moving points.
  //
  // Latitude spacing is about 27 feet per .00001 degree.
  // Longitude spacing varies, but the reality is that anything closer
  // than 27 feet does little but clutter the output.
  // As this is about the limit of consumer grade GPS, it seems a
  // reasonable tradeoff.

  return
    std::abs(wpta->latitude - wptb->latitude) < .00001 &&
    std::abs(wpta->longitude - wptb->longitude) < .00001 &&
    std::abs(wpta->altitude - wptb->altitude) < 20 &&
    (WAYPT_HAS(wpta,course) == WAYPT_HAS(wptb,course)) &&
    (wpta->course == wptb->course) &&
    (wpta->speed == wptb->speed) &&
    (wpta->heartrate == wptb->heartrate) &&
    (wpta->cadence == wptb->cadence) &&
    (wpta->temperature == wptb->temperature);
}

void TrackFilter::trackfilter_segment_head(const route_head* rte)
{
  queue* elem, *tmp;
  double avg_dist = 0;
  int index = 0;
  Waypoint* prev_wpt = nullptr;
  // Consider tossing trackpoints closer than this in radians.
  // (Empirically determined; It's a few dozen feet.)
  const double ktoo_close = 0.000005;

  QUEUE_FOR_EACH(&rte->waypoint_list, elem, tmp) {
    auto wpt = reinterpret_cast<Waypoint*>(elem);
    if (index > 0) {
      double cur_dist = gcdist(RAD(prev_wpt->latitude),
                               RAD(prev_wpt->longitude),
                               RAD(wpt->latitude),
                               RAD(wpt->longitude));
      // Denoise points that are on top of each other.
      if (avg_dist == 0) {
        avg_dist = cur_dist;
      }

      if (cur_dist < ktoo_close) {
        if (wpt != reinterpret_cast<Waypoint*>QUEUE_LAST(&rte->waypoint_list)) {
          auto next_wpt = reinterpret_cast<Waypoint*>QUEUE_NEXT(&wpt->Q);
          if (trackfilter_points_are_same(prev_wpt, wpt) &&
              trackfilter_points_are_same(wpt, next_wpt)) {
            track_del_wpt(const_cast<route_head*>(rte), wpt);
            continue;
          }
        }
      }
      if (cur_dist > .001 && cur_dist > 1.2* avg_dist) {
        avg_dist = cur_dist = 0;
        wpt->wpt_flags.new_trkseg = 1;
      }
      // Update weighted moving average;
      avg_dist = (cur_dist + 4.0 * avg_dist) / 5.0;
    }
    prev_wpt = wpt;
    index++;
  }
}

/*******************************************************************************
* global cb's
*******************************************************************************/

void TrackFilter::init()
{
  RteHdFunctor<TrackFilter> trackfilter_segment_head_f(this, &TrackFilter::trackfilter_segment_head);
  RteHdFunctor<TrackFilter> trackfilter_fill_track_list_cb_f(this, &TrackFilter::trackfilter_fill_track_list_cb);

  int count = track_count();

  /*
   * check time presence only if required. Options that NOT require time:
   *
   * - opt_title (!!! only if no format specifier is present !!!)
   * - opt_course
   * - opt_name
   */
  need_time = (
                opt_merge || opt_pack || opt_split || opt_sdistance ||
                opt_move || opt_fix || opt_speed ||
                (trackfilter_opt_count() == 0)	/* do pack by default */
              );
  /* in case of a formated title we also need valid timestamps */
  if ((opt_title != nullptr) && (strchr(opt_title, '%') != nullptr)) {
    need_time = true;
  }

  track_ct = 0;
  track_pts = 0;

  // Perform segmenting first.
  if (opt_segment) {
    track_disp_all(trackfilter_segment_head_f, nullptr, nullptr);
  }

  if (count > 0) {
    track_list = new trkflt_t[count];

    /* check all tracks for time and order (except merging) */

    track_disp_all(trackfilter_fill_track_list_cb_f, nullptr, nullptr);
    if (need_time) {
      qsort(track_list, track_ct, sizeof(*track_list), trackfilter_init_qsort_cb);
    }
  } else {
    track_list = nullptr;
  }
}

void TrackFilter::deinit()
{
  delete[] track_list;
  track_list = nullptr;
  track_ct = 0;
  track_pts = 0;
}

/*******************************************************************************
* trackfilter_process: called from gpsbabel central engine
*******************************************************************************/

void TrackFilter::process()
{
  RteHdFunctor<TrackFilter> trackfilter_minpoint_list_cb_f(this, &TrackFilter::trackfilter_minpoint_list_cb);

  if (track_ct == 0) {
    return;  /* no track(s), no fun */
  }

  int opts = trackfilter_opt_count();
  if (opts == 0) {
    opts = -1;  /* flag for do "pack" by default */
  }

  if (opt_name != nullptr) {
    if (--opts == 0) {
      return;
    }
  }

  if (opt_move != nullptr) {		/* Correct timestamps before any other op */
    trackfilter_move();
    if (--opts == 0) {
      return;
    }
  }

  if (opt_speed || opt_course || opt_fix) {
    trackfilter_synth();
    if (opt_speed) {
      opts--;
    }
    if (opt_course) {
      opts--;
    }
    if (opt_fix) {
      opts--;
    }
    if (!opts) {
      return;
    }
  }

  if ((opt_faketime != nullptr)) {
    opts--;

    trackfilter_faketime();

    if (opts == 0) {
      return;
    }

    deinit();       /* reinitialize */
    init();

    if (track_ct == 0) {
      return;  /* no more track(s), no more fun */
    }
  }

  if ((opt_stop != nullptr) || (opt_start != nullptr)) {
    if (opt_start != nullptr) {
      opts--;
    }
    if (opt_stop != nullptr) {
      opts--;
    }

    trackfilter_range();

    if (opts == 0) {
      return;
    }

    deinit();	/* reinitialize */
    init();

    if (track_ct == 0) {
      return;  /* no more track(s), no more fun */
    }

  }

  if (opt_seg2trk != nullptr) {
    trackfilter_seg2trk();
    if (--opts == 0) {
      return;
    }

    deinit();	/* reinitialize */
    init();
  }

  if (opt_trk2seg != nullptr) {
    trackfilter_trk2seg();
    if (--opts == 0) {
      return;
    }
  }

  if (opt_title != nullptr) {
    if (--opts == 0) {
      trackfilter_title();
      return;
    }
  }

  bool something_done = false;

  if ((opt_pack != nullptr) || (opts == -1)) {	/* call our default option */
    trackfilter_pack();
    something_done = true;
  } else if (opt_merge != nullptr) {
    trackfilter_merge();
    something_done = true;
  }

  if (something_done && (--opts <= 0)) {
    if (opt_title != nullptr) {
      trackfilter_title();
    }
    return;
  }

  if ((opt_split != nullptr) || (opt_sdistance != nullptr)) {
    if (track_ct > 1) {
      fatal(MYNAME "-split: Cannot split more than one track, please pack (or merge) before!\n");
    }

    trackfilter_split();
  }

  // Performed last as previous options may have created "small" tracks.
  if ((opt_minpoints != nullptr) && atoi(opt_minpoints) > 0) {
    track_disp_all(trackfilter_minpoint_list_cb_f, nullptr, nullptr);
  }
}

#endif // FILTERS_ENABLED
