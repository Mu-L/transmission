// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::max()
#include <cstring> // strchr()
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <glibmm.h>
#include <glibmm/i18n.h>

#include <fmt/core.h>

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h> /* tr_truncd() */

#include "HigWorkarea.h" // GUI_PAD, GUI_PAD_SMALL
#include "IconCache.h"
#include "TorrentCellRenderer.h"
#include "Utils.h"

/* #define TEST_RTL */

/***
****
***/

#define REQ_HEIGHT(Obj) IF_GTKMM4((Obj).get_height(), (Obj).height)
#define REQ_WIDTH(Obj) IF_GTKMM4((Obj).get_width(), (Obj).width)

namespace
{

auto const DefaultBarHeight = 12;
auto const CompactBarWidth = 50;
auto const SmallScale = 0.9;
auto const CompactIconSize = IF_GTKMM4(Gtk::IconSize::NORMAL, Gtk::ICON_SIZE_MENU);
auto const FullIconSize = IF_GTKMM4(Gtk::IconSize::LARGE, Gtk::ICON_SIZE_DND);

auto getProgressString(tr_torrent const* tor, uint64_t total_size, tr_stat const* st)
{
    Glib::ustring gstr;

    bool const isDone = st->leftUntilDone == 0;
    uint64_t const haveTotal = st->haveUnchecked + st->haveValid;
    bool const isSeed = st->haveValid >= total_size;
    double seedRatio;
    bool const hasSeedRatio = tr_torrentGetSeedRatio(tor, &seedRatio);

    if (!isDone) // downloading
    {
        // 50 MB of 200 MB (25%)
        gstr += fmt::format(
            _("{current_size} of {complete_size} ({percent_done}%)"),
            fmt::arg("current_size", tr_strlsize(haveTotal)),
            fmt::arg("complete_size", tr_strlsize(st->sizeWhenDone)),
            fmt::arg("percent_done", tr_strpercent(st->percentDone * 100.0)));
    }
    else if (!isSeed && hasSeedRatio) // partial seed, seed ratio
    {
        // 50 MB of 200 MB (25%), uploaded 30 MB (Ratio: X%, Goal: Y%)
        gstr += fmt::format(
            // xgettext:no-c-format
            _("{current_size} of {complete_size} ({percent_complete}%), uploaded {uploaded_size} (Ratio: {ratio}, Goal: {seed_ratio})"),
            fmt::arg("current_size", tr_strlsize(haveTotal)),
            fmt::arg("complete_size", tr_strlsize(total_size)),
            fmt::arg("percent_complete", tr_strpercent(st->percentComplete * 100.0)),
            fmt::arg("uploaded_size", tr_strlsize(st->uploadedEver)),
            fmt::arg("ratio", tr_strlratio(st->ratio)),
            fmt::arg("seed_ratio", tr_strlratio(seedRatio)));
    }
    else if (!isSeed) // partial seed, no seed ratio
    {
        gstr += fmt::format(
            // xgettext:no-c-format
            _("{current_size} of {complete_size} ({percent_complete}%), uploaded {uploaded_size} (Ratio: {ratio})"),
            fmt::arg("current_size", tr_strlsize(haveTotal)),
            fmt::arg("complete_size", tr_strlsize(total_size)),
            fmt::arg("percent_complete", tr_strpercent(st->percentComplete * 100.0)),
            fmt::arg("uploaded_size", tr_strlsize(st->uploadedEver)),
            fmt::arg("ratio", tr_strlratio(st->ratio)));
    }
    else if (hasSeedRatio) // seed, seed ratio
    {
        gstr += fmt::format(
            _("{complete_size}, uploaded {uploaded_size} (Ratio: {ratio}, Goal: {seed_ratio})"),
            fmt::arg("complete_size", tr_strlsize(total_size)),
            fmt::arg("uploaded_size", tr_strlsize(st->uploadedEver)),
            fmt::arg("ratio", tr_strlratio(st->ratio)),
            fmt::arg("seed_ratio", tr_strlratio(seedRatio)));
    }
    else // seed, no seed ratio
    {
        gstr += fmt::format(
            _("{complete_size}, uploaded {uploaded_size} (Ratio: {ratio})"),
            fmt::arg("complete_size", tr_strlsize(total_size)),
            fmt::arg("uploaded_size", tr_strlsize(st->uploadedEver)),
            fmt::arg("ratio", tr_strlratio(st->ratio)));
    }

    // add time remaining when applicable
    if (st->activity == TR_STATUS_DOWNLOAD || (hasSeedRatio && st->activity == TR_STATUS_SEED))
    {
        int const eta = st->eta;
        gstr += " - ";

        if (eta < 0)
        {
            gstr += _("Remaining time unknown");
        }
        else
        {
            gstr += tr_format_time_left(eta);
        }
    }

    return gstr;
}

std::string getShortTransferString(
    tr_torrent const* const tor,
    tr_stat const* const st,
    double uploadSpeed_KBps,
    double downloadSpeed_KBps)
{
    bool const have_meta = tr_torrentHasMetadata(tor);

    if (bool const have_down = have_meta && (st->peersSendingToUs > 0 || st->webseedsSendingToUs > 0); have_down)
    {
        return fmt::format(
            _("{download_speed} ▼  {upload_speed} ▲"),
            fmt::arg("upload_speed", tr_formatter_speed_KBps(uploadSpeed_KBps)),
            fmt::arg("download_speed", tr_formatter_speed_KBps(downloadSpeed_KBps)));
    }

    if (bool const have_up = have_meta && st->peersGettingFromUs > 0; have_up)
    {
        return fmt::format(_("{upload_speed} ▲"), fmt::arg("upload_speed", tr_formatter_speed_KBps(uploadSpeed_KBps)));
    }

    if (st->isStalled)
    {
        return _("Stalled");
    }

    return {};
}

std::string getShortStatusString(
    tr_torrent const* const tor,
    tr_stat const* const st,
    double uploadSpeed_KBps,
    double downloadSpeed_KBps)
{
    switch (st->activity)
    {
    case TR_STATUS_STOPPED:
        return st->finished ? _("Finished") : _("Paused");

    case TR_STATUS_CHECK_WAIT:
        return _("Queued for verification");

    case TR_STATUS_DOWNLOAD_WAIT:
        return _("Queued for download");

    case TR_STATUS_SEED_WAIT:
        return _("Queued for seeding");

    case TR_STATUS_CHECK:
        return fmt::format(
            // xgettext:no-c-format
            _("Verifying local data ({percent_done}% tested)"),
            fmt::arg("percent_done", tr_truncd(st->recheckProgress * 100.0, 1)));

    case TR_STATUS_DOWNLOAD:
    case TR_STATUS_SEED:
        return fmt::format(
            FMT_STRING("{:s} {:s}"),
            getShortTransferString(tor, st, uploadSpeed_KBps, downloadSpeed_KBps),
            fmt::format(_("Ratio: {ratio}"), fmt::arg("ratio", tr_strlratio(st->ratio))));

    default:
        return {};
    }
}

std::optional<std::string> getErrorString(tr_stat const* st)
{
    switch (st->error)
    {
    case TR_STAT_TRACKER_WARNING:
        return fmt::format(_("Tracker warning: '{warning}'"), fmt::arg("warning", st->errorString));

    case TR_STAT_TRACKER_ERROR:
        return fmt::format(_("Tracker Error: '{error}'"), fmt::arg("error", st->errorString));

    case TR_STAT_LOCAL_ERROR:
        return fmt::format(_("Local error: '{error}'"), fmt::arg("error", st->errorString));

    default:
        return std::nullopt;
    }
}

auto getActivityString(
    tr_torrent const* const tor,
    tr_stat const* const st,
    double const uploadSpeed_KBps,
    double const downloadSpeed_KBps)
{
    switch (st->activity)
    {
    case TR_STATUS_STOPPED:
    case TR_STATUS_CHECK_WAIT:
    case TR_STATUS_CHECK:
    case TR_STATUS_DOWNLOAD_WAIT:
    case TR_STATUS_SEED_WAIT:
        return getShortStatusString(tor, st, uploadSpeed_KBps, downloadSpeed_KBps);

    case TR_STATUS_DOWNLOAD:
        if (!tr_torrentHasMetadata(tor))
        {
            return fmt::format(
                ngettext(
                    // xgettext:no-c-format
                    "Downloading metadata from {active_count} connected peer ({percent_done}% done)",
                    "Downloading metadata from {active_count} connected peers ({percent_done}% done)",
                    st->peersConnected),
                fmt::arg("active_count", st->peersConnected),
                fmt::arg("percent_done", tr_strpercent(st->metadataPercentComplete * 100.0)));
        }

        if (st->peersSendingToUs != 0 && st->webseedsSendingToUs != 0)
        {
            return fmt::format(
                ngettext(
                    "Downloading from {active_count} of {connected_count} connected peer and webseed",
                    "Downloading from {active_count} of {connected_count} connected peers and webseeds",
                    st->peersConnected + st->webseedsSendingToUs),
                fmt::arg("active_count", st->peersSendingToUs + st->webseedsSendingToUs),
                fmt::arg("connected_count", st->peersConnected + st->webseedsSendingToUs));
        }

        if (st->webseedsSendingToUs != 0)
        {
            return fmt::format(
                ngettext(
                    "Downloading from {active_count} webseed",
                    "Downloading from {active_count} webseeds",
                    st->webseedsSendingToUs),
                fmt::arg("active_count", st->webseedsSendingToUs));
        }

        return fmt::format(
            ngettext(
                "Downloading from {active_count} of {connected_count} connected peer",
                "Downloading from {active_count} of {connected_count} connected peers",
                st->peersConnected),
            fmt::arg("active_count", st->peersSendingToUs),
            fmt::arg("connected_count", st->peersConnected));

    case TR_STATUS_SEED:
        return fmt::format(
            ngettext(
                "Seeding to {active_count} of {connected_count} connected peer",
                "Seeding to {active_count} of {connected_count} connected peers",
                st->peersConnected),
            fmt::arg("active_count", st->peersGettingFromUs),
            fmt::arg("connected_count", st->peersConnected));

    default:
        g_assert_not_reached();
        return std::string{};
    }
}

std::string getStatusString(
    tr_torrent const* tor,
    tr_stat const* st,
    double const uploadSpeed_KBps,
    double const downloadSpeed_KBps,
    bool ignore_errors = false)
{
    auto status_str = (ignore_errors ? std::nullopt : getErrorString(st))
                          .value_or(getActivityString(tor, st, uploadSpeed_KBps, downloadSpeed_KBps));

    if (st->activity != TR_STATUS_CHECK_WAIT && st->activity != TR_STATUS_CHECK && st->activity != TR_STATUS_DOWNLOAD_WAIT &&
        st->activity != TR_STATUS_SEED_WAIT && st->activity != TR_STATUS_STOPPED)
    {
        if (auto const buf = getShortTransferString(tor, st, uploadSpeed_KBps, downloadSpeed_KBps); !std::empty(buf))
        {
            status_str += fmt::format(FMT_STRING(" - {:s}"), buf);
        }
    }

    return status_str;
}

} // namespace

/***
****
***/

class TorrentCellRenderer::Impl
{
    using SnapshotPtr = TorrentCellRenderer::SnapshotPtr;
    using IconSize = IF_GTKMM4(Gtk::IconSize, Gtk::BuiltinIconSize);

public:
    explicit Impl(TorrentCellRenderer& renderer);
    ~Impl();

    TR_DISABLE_COPY_MOVE(Impl)

    void get_size_compact(Gtk::Widget& widget, int& width, int& height) const;
    void get_size_full(Gtk::Widget& widget, int& width, int& height) const;

    void render_compact(
        SnapshotPtr const& snapshot,
        Gtk::Widget& widget,
        Gdk::Rectangle const& background_area,
        Gtk::CellRendererState flags);
    void render_full(
        SnapshotPtr const& snapshot,
        Gtk::Widget& widget,
        Gdk::Rectangle const& background_area,
        Gtk::CellRendererState flags);

public:
    Glib::Property<gpointer> torrent;
    Glib::Property<int> bar_height;

    /* Use this instead of tr_stat.pieceUploadSpeed so that the model can
       control when the speed displays get updated. This is done to keep
       the individual torrents' speeds and the status bar's overall speed
       in sync even if they refresh at slightly different times */
    Glib::Property<double> upload_speed_KBps;

    /* @see upload_speed_Bps */
    Glib::Property<double> download_speed_KBps;

    Glib::Property<bool> compact;

private:
    void render_progress_bar(
        SnapshotPtr const& snapshot,
        Gtk::Widget& widget,
        Gdk::Rectangle const& area,
        Gtk::CellRendererState flags,
        Gdk::RGBA const& color);

    static void set_icon(Gtk::CellRendererPixbuf& renderer, Glib::RefPtr<Gio::Icon> const& icon, IconSize icon_size);
    static void adjust_progress_bar_hue(
        Cairo::RefPtr<Cairo::Surface> const& bg_surface,
        Cairo::RefPtr<Cairo::Context> const& context,
        Gdk::RGBA const& color,
        Gdk::Rectangle const& area,
        double bg_x,
        double bg_y);

private:
    TorrentCellRenderer& renderer_;

    Gtk::CellRendererText* text_renderer_ = nullptr;
    Gtk::CellRendererProgress* progress_renderer_ = nullptr;
    Gtk::CellRendererPixbuf* icon_renderer_ = nullptr;
};

/***
****
***/

namespace
{

Glib::RefPtr<Gio::Icon> get_icon(tr_torrent const* tor)
{
    auto mime_type = std::string_view{};

    if (auto const n_files = tr_torrentFileCount(tor); n_files == 0)
    {
        mime_type = UnknownMimeType;
    }
    else if (n_files > 1)
    {
        mime_type = DirectoryMimeType;
    }
    else
    {
        auto const* const name = tr_torrentFile(tor, 0).name;

        mime_type = strchr(name, '/') != nullptr ? DirectoryMimeType : tr_get_mime_type_for_filename(name);
    }

    return gtr_get_mime_type_icon(mime_type);
}

} // namespace

/***
****
***/

void TorrentCellRenderer::Impl::set_icon(
    Gtk::CellRendererPixbuf& renderer,
    Glib::RefPtr<Gio::Icon> const& icon,
    IconSize icon_size)
{
    renderer.property_gicon() = icon;
#if GTKMM_CHECK_VERSION(4, 0, 0)
    renderer.property_icon_size() = icon_size;
#else
    renderer.property_stock_size() = icon_size;
#endif
}

void TorrentCellRenderer::Impl::get_size_compact(Gtk::Widget& widget, int& width, int& height) const
{
    int xpad;
    int ypad;
    Gtk::Requisition min_size;
    Gtk::Requisition icon_size;
    Gtk::Requisition name_size;
    Gtk::Requisition stat_size;

    auto* const tor = static_cast<tr_torrent*>(torrent.get_value());
    auto const* const st = tr_torrentStatCached(tor);

    auto const icon = get_icon(tor);
    auto const name = Glib::ustring(tr_torrentName(tor));
    auto const gstr_stat = getShortStatusString(tor, st, upload_speed_KBps.get_value(), download_speed_KBps.get_value());
    renderer_.get_padding(xpad, ypad);

    /* get the idealized cell dimensions */
    set_icon(*icon_renderer_, icon, CompactIconSize);
    icon_renderer_->get_preferred_size(widget, min_size, icon_size);
    text_renderer_->property_text() = name;
    text_renderer_->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(NONE);
    text_renderer_->property_scale() = 1.0;
    text_renderer_->get_preferred_size(widget, min_size, name_size);
    text_renderer_->property_text() = gstr_stat;
    text_renderer_->property_scale() = SmallScale;
    text_renderer_->get_preferred_size(widget, min_size, stat_size);

    /**
    *** LAYOUT
    **/

    width = xpad * 2 + REQ_WIDTH(icon_size) + GUI_PAD + CompactBarWidth + GUI_PAD + REQ_WIDTH(stat_size);
    height = ypad * 2 + std::max(REQ_HEIGHT(name_size), bar_height.get_value());
}

void TorrentCellRenderer::Impl::get_size_full(Gtk::Widget& widget, int& width, int& height) const
{
    int xpad;
    int ypad;
    Gtk::Requisition min_size;
    Gtk::Requisition icon_size;
    Gtk::Requisition name_size;
    Gtk::Requisition stat_size;
    Gtk::Requisition prog_size;

    auto* const tor = static_cast<tr_torrent*>(torrent.get_value());
    auto const* const st = tr_torrentStatCached(tor);
    auto const total_size = tr_torrentTotalSize(tor);

    auto const icon = get_icon(tor);
    auto const name = Glib::ustring(tr_torrentName(tor));
    auto const gstr_stat = getStatusString(tor, st, upload_speed_KBps.get_value(), download_speed_KBps.get_value(), true);
    auto const gstr_prog = getProgressString(tor, total_size, st);
    renderer_.get_padding(xpad, ypad);

    /* get the idealized cell dimensions */
    set_icon(*icon_renderer_, icon, FullIconSize);
    icon_renderer_->get_preferred_size(widget, min_size, icon_size);
    text_renderer_->property_text() = name;
    text_renderer_->property_weight() = TR_PANGO_WEIGHT(BOLD);
    text_renderer_->property_scale() = 1.0;
    text_renderer_->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(NONE);
    text_renderer_->get_preferred_size(widget, min_size, name_size);
    text_renderer_->property_text() = gstr_prog;
    text_renderer_->property_weight() = TR_PANGO_WEIGHT(NORMAL);
    text_renderer_->property_scale() = SmallScale;
    text_renderer_->get_preferred_size(widget, min_size, prog_size);
    text_renderer_->property_text() = gstr_stat;
    text_renderer_->get_preferred_size(widget, min_size, stat_size);

    /**
    *** LAYOUT
    **/

    width = xpad * 2 + REQ_WIDTH(icon_size) + GUI_PAD + std::max(REQ_WIDTH(prog_size), REQ_WIDTH(stat_size));
    height = ypad * 2 + REQ_HEIGHT(name_size) + REQ_HEIGHT(prog_size) + GUI_PAD_SMALL + bar_height.get_value() + GUI_PAD_SMALL +
        REQ_HEIGHT(stat_size);
}

void TorrentCellRenderer::get_preferred_width_vfunc(Gtk::Widget& widget, int& minimum_width, int& natural_width) const
{
    if (impl_->torrent.get_value() != nullptr)
    {
        int w;
        int h;

        if (impl_->compact.get_value())
        {
            impl_->get_size_compact(widget, w, h);
        }
        else
        {
            impl_->get_size_full(widget, w, h);
        }

        minimum_width = w;
        natural_width = w;
    }
}

void TorrentCellRenderer::get_preferred_height_vfunc(Gtk::Widget& widget, int& minimum_height, int& natural_height) const
{
    if (impl_->torrent.get_value() != nullptr)
    {
        int w;
        int h;

        if (impl_->compact.get_value())
        {
            impl_->get_size_compact(widget, w, h);
        }
        else
        {
            impl_->get_size_full(widget, w, h);
        }

        minimum_height = h;
        natural_height = h;
    }
}

namespace
{

double get_percent_done(tr_torrent const* tor, tr_stat const* st, bool* seed)
{
    double d;

    if (st->activity == TR_STATUS_SEED && tr_torrentGetSeedRatio(tor, &d))
    {
        *seed = true;
        d = MAX(0.0, st->seedRatioPercentDone);
    }
    else
    {
        *seed = false;
        d = MAX(0.0, st->percentDone);
    }

    return d;
}

Gdk::RGBA const& get_progress_bar_color(tr_stat const& st)
{
    static auto const steelblue_color = Gdk::RGBA("steelblue");
    static auto const forestgreen_color = Gdk::RGBA("forestgreen");
    static auto const silver_color = Gdk::RGBA("silver");

    return st.activity == TR_STATUS_DOWNLOAD ? steelblue_color :
                                               (st.activity == TR_STATUS_SEED ? forestgreen_color : silver_color);
}

Cairo::RefPtr<Cairo::Surface> get_mask_surface(Cairo::RefPtr<Cairo::Surface> const& surface, Gdk::Rectangle const& area)
{
    auto const mask_surface = Cairo::ImageSurface::create(TR_CAIRO_SURFACE_FORMAT(A8), area.get_width(), area.get_height());
    auto const mask_context = Cairo::Context::create(mask_surface);

    mask_context->set_source_rgb(0, 0, 0);
    mask_context->rectangle(area.get_x(), area.get_y(), area.get_width(), area.get_height());
    mask_context->fill();

    mask_context->set_operator(TR_CAIRO_CONTEXT_OPERATOR(CLEAR));
    mask_context->mask(surface, area.get_x(), area.get_y());
    mask_context->fill();

    return mask_surface;
}

template<typename... Ts>
void render_impl(Gtk::CellRenderer& renderer, Ts&&... args)
{
    renderer.IF_GTKMM4(snapshot, render)(std::forward<Ts>(args)...);
}

} // namespace

void TorrentCellRenderer::Impl::adjust_progress_bar_hue(
    Cairo::RefPtr<Cairo::Surface> const& bg_surface,
    Cairo::RefPtr<Cairo::Context> const& context,
    Gdk::RGBA const& color,
    Gdk::Rectangle const& area,
    double bg_x,
    double bg_y)
{
    using TrCairoContextOperator = IF_GTKMM4(Cairo::Context::Operator, Cairo::Operator);

    auto const mask_surface = get_mask_surface(context->get_target(), area);

    // Add background under the progress bar, for better results around the transparent areas
    context->set_source(bg_surface, bg_x, bg_y);
    context->set_operator(TR_CAIRO_CONTEXT_OPERATOR(DEST_OVER));
    context->rectangle(area.get_x(), area.get_y(), area.get_width(), area.get_height());
    context->fill();

    // Adjust surface color
    context->set_source_rgb(color.get_red(), color.get_green(), color.get_blue());
    context->set_operator(static_cast<TrCairoContextOperator>(CAIRO_OPERATOR_HSL_COLOR));
    context->rectangle(area.get_x(), area.get_y(), area.get_width(), area.get_height());
    context->fill();

    // Clear out masked (fully transparent) areas
    context->set_operator(TR_CAIRO_CONTEXT_OPERATOR(CLEAR));
    context->mask(mask_surface, area.get_x(), area.get_y());
    context->fill();
}

void TorrentCellRenderer::Impl::render_progress_bar(
    SnapshotPtr const& snapshot,
    Gtk::Widget& widget,
    Gdk::Rectangle const& area,
    Gtk::CellRendererState flags,
    Gdk::RGBA const& color)
{
    auto const temp_area = Gdk::Rectangle(0, 0, area.get_width(), area.get_height());
    auto const temp_surface = Cairo::ImageSurface::create(TR_CAIRO_SURFACE_FORMAT(ARGB32), area.get_width(), area.get_height());
    auto const temp_context = Cairo::Context::create(temp_surface);

    {
#if GTKMM_CHECK_VERSION(4, 0, 0)
        auto const temp_snapshot = Gtk::Snapshot::create();
#endif

        render_impl(*progress_renderer_, IF_GTKMM4(temp_snapshot, temp_context), widget, temp_area, temp_area, flags);

#if GTKMM_CHECK_VERSION(4, 0, 0)
        temp_snapshot->reference();
        auto const render_node = std::unique_ptr<GskRenderNode, void (*)(GskRenderNode*)>(
            gtk_snapshot_free_to_node(Glib::unwrap(temp_snapshot)),
            [](GskRenderNode* p) { gsk_render_node_unref(p); });
        gsk_render_node_draw(render_node.get(), temp_context->cobj());
#endif
    }

#if GTKMM_CHECK_VERSION(4, 0, 0)
    auto const context = snapshot->append_cairo(area);
    auto const surface = context->get_target();
#else
    auto const context = snapshot;
    auto const surface = Cairo::Surface::create(
        context->get_target(),
        area.get_x(),
        area.get_y(),
        area.get_width(),
        area.get_height());
#endif

    double dx = 0;
    double dy = 0;
    context->device_to_user(dx, dy);

    adjust_progress_bar_hue(surface, temp_context, color, temp_area, dx - area.get_x(), dy - area.get_y());

    context->set_source(temp_context->get_target(), area.get_x(), area.get_y());
    context->rectangle(area.get_x(), area.get_y(), area.get_width(), area.get_height());
    context->fill();
}

void TorrentCellRenderer::Impl::render_compact(
    SnapshotPtr const& snapshot,
    Gtk::Widget& widget,
    Gdk::Rectangle const& background_area,
    Gtk::CellRendererState flags)
{
    int xpad;
    int ypad;
    int min_width;
    int width;
    bool seed;

    auto* const tor = static_cast<tr_torrent*>(torrent.get_value());
    auto const* const st = tr_torrentStatCached(tor);
    bool const active = st->activity != TR_STATUS_STOPPED && st->activity != TR_STATUS_DOWNLOAD_WAIT &&
        st->activity != TR_STATUS_SEED_WAIT;
    auto const percentDone = get_percent_done(tor, st, &seed);
    bool const sensitive = active || st->error != 0;

    if (st->activity == TR_STATUS_STOPPED)
    {
        flags |= TR_GTK_CELL_RENDERER_STATE(INSENSITIVE);
    }

    if (st->error != 0 && (flags & TR_GTK_CELL_RENDERER_STATE(SELECTED)) == Gtk::CellRendererState{})
    {
        text_renderer_->property_foreground() = "red";
    }
    else
    {
        text_renderer_->property_foreground_set() = false;
    }

    auto const icon = get_icon(tor);
    auto const name = Glib::ustring(tr_torrentName(tor));
    auto const& progress_color = get_progress_bar_color(*st);
    auto const gstr_stat = getShortStatusString(tor, st, upload_speed_KBps.get_value(), download_speed_KBps.get_value());
    renderer_.get_padding(xpad, ypad);

    auto fill_area = background_area;
    fill_area.set_x(fill_area.get_x() + xpad);
    fill_area.set_y(fill_area.get_y() + ypad);
    fill_area.set_width(fill_area.get_width() - xpad * 2);
    fill_area.set_height(fill_area.get_height() - ypad * 2);

    auto icon_area = fill_area;
    set_icon(*icon_renderer_, icon, CompactIconSize);
    icon_renderer_->get_preferred_width(widget, min_width, width);
    icon_area.set_width(width);

    auto prog_area = fill_area;
    prog_area.set_width(CompactBarWidth);

    auto stat_area = fill_area;
    text_renderer_->property_text() = gstr_stat;
    text_renderer_->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(NONE);
    text_renderer_->property_scale() = SmallScale;
    text_renderer_->get_preferred_width(widget, min_width, width);
    stat_area.set_width(width);

    auto name_area = fill_area;
    name_area.set_width(
        fill_area.get_width() - icon_area.get_width() - stat_area.get_width() - prog_area.get_width() - GUI_PAD * 3);

    if ((renderer_.get_state(widget, flags) & TR_GTK_STATE_FLAGS(DIR_RTL)) == Gtk::StateFlags{})
    {
        icon_area.set_x(fill_area.get_x());
        prog_area.set_x(fill_area.get_x() + fill_area.get_width() - prog_area.get_width());
        stat_area.set_x(prog_area.get_x() - stat_area.get_width() - GUI_PAD);
        name_area.set_x(icon_area.get_x() + icon_area.get_width() + GUI_PAD);
    }
    else
    {
        icon_area.set_x(fill_area.get_x() + fill_area.get_width() - icon_area.get_width());
        prog_area.set_x(fill_area.get_x());
        stat_area.set_x(prog_area.get_x() + prog_area.get_width() + GUI_PAD);
        name_area.set_x(stat_area.get_x() + stat_area.get_width() + GUI_PAD);
    }

    /**
    *** RENDER
    **/

    set_icon(*icon_renderer_, icon, CompactIconSize);
    icon_renderer_->property_sensitive() = sensitive;
    render_impl(*icon_renderer_, snapshot, widget, icon_area, icon_area, flags);

    auto const percent_done = static_cast<int>(percentDone * 100.0);
    progress_renderer_->property_value() = percent_done;
    progress_renderer_->property_text() = fmt::format(FMT_STRING("{:d}%"), percent_done);
    progress_renderer_->property_sensitive() = sensitive;
    render_progress_bar(snapshot, widget, prog_area, flags, progress_color);

    text_renderer_->property_text() = gstr_stat;
    text_renderer_->property_scale() = SmallScale;
    text_renderer_->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(END);
    render_impl(*text_renderer_, snapshot, widget, stat_area, stat_area, flags);

    text_renderer_->property_text() = name;
    text_renderer_->property_scale() = 1.0;
    render_impl(*text_renderer_, snapshot, widget, name_area, name_area, flags);
}

void TorrentCellRenderer::Impl::render_full(
    SnapshotPtr const& snapshot,
    Gtk::Widget& widget,
    Gdk::Rectangle const& background_area,
    Gtk::CellRendererState flags)
{
    int xpad;
    int ypad;
    Gtk::Requisition min_size;
    Gtk::Requisition size;
    bool seed;

    auto* const tor = static_cast<tr_torrent*>(torrent.get_value());
    auto const* const st = tr_torrentStatCached(tor);
    auto const total_size = tr_torrentTotalSize(tor);
    bool const active = st->activity != TR_STATUS_STOPPED && st->activity != TR_STATUS_DOWNLOAD_WAIT &&
        st->activity != TR_STATUS_SEED_WAIT;
    auto const percentDone = get_percent_done(tor, st, &seed);
    bool const sensitive = active || st->error != 0;

    if (st->activity == TR_STATUS_STOPPED)
    {
        flags |= TR_GTK_CELL_RENDERER_STATE(INSENSITIVE);
    }

    if (st->error != 0 && (flags & TR_GTK_CELL_RENDERER_STATE(SELECTED)) == Gtk::CellRendererState{})
    {
        text_renderer_->property_foreground() = "red";
    }
    else
    {
        text_renderer_->property_foreground_set() = false;
    }

    auto const icon = get_icon(tor);
    auto const name = Glib::ustring(tr_torrentName(tor));
    auto const& progress_color = get_progress_bar_color(*st);
    auto const gstr_prog = getProgressString(tor, total_size, st);
    auto const gstr_stat = getStatusString(tor, st, upload_speed_KBps.get_value(), download_speed_KBps.get_value());
    renderer_.get_padding(xpad, ypad);

    /* get the idealized cell dimensions */
    Gdk::Rectangle icon_area;
    set_icon(*icon_renderer_, icon, FullIconSize);
    icon_renderer_->get_preferred_size(widget, min_size, size);
    icon_area.set_width(REQ_WIDTH(size));
    icon_area.set_height(REQ_HEIGHT(size));

    Gdk::Rectangle name_area;
    text_renderer_->property_text() = name;
    text_renderer_->property_weight() = TR_PANGO_WEIGHT(BOLD);
    text_renderer_->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(NONE);
    text_renderer_->property_scale() = 1.0;
    text_renderer_->get_preferred_size(widget, min_size, size);
    name_area.set_height(REQ_HEIGHT(size));

    Gdk::Rectangle prog_area;
    text_renderer_->property_text() = gstr_prog;
    text_renderer_->property_weight() = TR_PANGO_WEIGHT(NORMAL);
    text_renderer_->property_scale() = SmallScale;
    text_renderer_->get_preferred_size(widget, min_size, size);
    prog_area.set_height(REQ_HEIGHT(size));

    Gdk::Rectangle stat_area;
    text_renderer_->property_text() = gstr_stat;
    text_renderer_->get_preferred_size(widget, min_size, size);
    stat_area.set_height(REQ_HEIGHT(size));

    Gdk::Rectangle prct_area;

    /**
    *** LAYOUT
    **/

    auto fill_area = background_area;
    fill_area.set_x(fill_area.get_x() + xpad);
    fill_area.set_y(fill_area.get_y() + ypad);
    fill_area.set_width(fill_area.get_width() - xpad * 2);
    fill_area.set_height(fill_area.get_height() - ypad * 2);

    /* icon */
    icon_area.set_y(fill_area.get_y() + (fill_area.get_height() - icon_area.get_height()) / 2);

    /* name */
    name_area.set_y(fill_area.get_y());
    name_area.set_width(fill_area.get_width() - GUI_PAD - icon_area.get_width());

    if ((renderer_.get_state(widget, flags) & TR_GTK_STATE_FLAGS(DIR_RTL)) == Gtk::StateFlags{})
    {
        icon_area.set_x(fill_area.get_x());
        name_area.set_x(fill_area.get_x() + fill_area.get_width() - name_area.get_width());
    }
    else
    {
        icon_area.set_x(fill_area.get_x() + fill_area.get_width() - icon_area.get_width());
        name_area.set_x(fill_area.get_x());
    }

    /* prog */
    prog_area.set_x(name_area.get_x());
    prog_area.set_y(name_area.get_y() + name_area.get_height());
    prog_area.set_width(name_area.get_width());

    /* progressbar */
    prct_area.set_x(prog_area.get_x());
    prct_area.set_y(prog_area.get_y() + prog_area.get_height() + GUI_PAD_SMALL);
    prct_area.set_width(prog_area.get_width());
    prct_area.set_height(bar_height.get_value());

    /* status */
    stat_area.set_x(prct_area.get_x());
    stat_area.set_y(prct_area.get_y() + prct_area.get_height() + GUI_PAD_SMALL);
    stat_area.set_width(prct_area.get_width());

    /**
    *** RENDER
    **/

    set_icon(*icon_renderer_, icon, FullIconSize);
    icon_renderer_->property_sensitive() = sensitive;
    render_impl(*icon_renderer_, snapshot, widget, icon_area, icon_area, flags);

    text_renderer_->property_text() = name;
    text_renderer_->property_scale() = 1.0;
    text_renderer_->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(END);
    text_renderer_->property_weight() = TR_PANGO_WEIGHT(BOLD);
    render_impl(*text_renderer_, snapshot, widget, name_area, name_area, flags);

    text_renderer_->property_text() = gstr_prog;
    text_renderer_->property_scale() = SmallScale;
    text_renderer_->property_weight() = TR_PANGO_WEIGHT(NORMAL);
    render_impl(*text_renderer_, snapshot, widget, prog_area, prog_area, flags);

    progress_renderer_->property_value() = static_cast<int>(percentDone * 100.0);
    progress_renderer_->property_text() = Glib::ustring();
    progress_renderer_->property_sensitive() = sensitive;
    render_progress_bar(snapshot, widget, prct_area, flags, progress_color);

    text_renderer_->property_text() = gstr_stat;
    render_impl(*text_renderer_, snapshot, widget, stat_area, stat_area, flags);
}

void TorrentCellRenderer::IF_GTKMM4(snapshot_vfunc, render_vfunc)(
    SnapshotPtr const& snapshot,
    Gtk::Widget& widget,
    Gdk::Rectangle const& background_area,
    Gdk::Rectangle const& /*cell_area*/,
    Gtk::CellRendererState flags)
{
#ifdef TEST_RTL
    auto const real_dir = widget.get_direction();
    widget.set_direction(Gtk::TEXT_DIR_RTL);
#endif

    if (impl_->torrent.get_value() != nullptr)
    {
        if (impl_->compact.get_value())
        {
            impl_->render_compact(snapshot, widget, background_area, flags);
        }
        else
        {
            impl_->render_full(snapshot, widget, background_area, flags);
        }
    }

#ifdef TEST_RTL
    widget.set_direction(real_dir);
#endif
}

TorrentCellRenderer::Impl::~Impl()
{
    text_renderer_->unreference();
    progress_renderer_->unreference();
    icon_renderer_->unreference();
}

TorrentCellRenderer::TorrentCellRenderer()
    : Glib::ObjectBase(typeid(TorrentCellRenderer))
    , impl_(std::make_unique<Impl>(*this))
{
}

TorrentCellRenderer::~TorrentCellRenderer() = default;

TorrentCellRenderer::Impl::Impl(TorrentCellRenderer& renderer)
    : torrent(renderer, "torrent", nullptr)
    , bar_height(renderer, "bar-height", DefaultBarHeight)
    , upload_speed_KBps(renderer, "piece-upload-speed", 0)
    , download_speed_KBps(renderer, "piece-download-speed", 0)
    , compact(renderer, "compact", false)
    , renderer_(renderer)
{
    text_renderer_ = Gtk::make_managed<Gtk::CellRendererText>();
    text_renderer_->property_xpad() = 0;
    text_renderer_->property_ypad() = 0;

    progress_renderer_ = Gtk::make_managed<Gtk::CellRendererProgress>();
    icon_renderer_ = Gtk::make_managed<Gtk::CellRendererPixbuf>();
}

Glib::PropertyProxy<gpointer> TorrentCellRenderer::property_torrent()
{
    return impl_->torrent.get_proxy();
}

Glib::PropertyProxy<double> TorrentCellRenderer::property_piece_upload_speed()
{
    return impl_->upload_speed_KBps.get_proxy();
}

Glib::PropertyProxy<double> TorrentCellRenderer::property_piece_download_speed()
{
    return impl_->download_speed_KBps.get_proxy();
}

Glib::PropertyProxy<int> TorrentCellRenderer::property_bar_height()
{
    return impl_->bar_height.get_proxy();
}

Glib::PropertyProxy<bool> TorrentCellRenderer::property_compact()
{
    return impl_->compact.get_proxy();
}
