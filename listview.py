from pathlib import Path
import sys

import paho.mqtt.client as mqtt
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.widgets import Button, Footer, Header, Label, ListItem, ListView, Static

sys.path.insert(0, str(Path(__file__).parent))
from conductor.config import MIDI_DIR, MQTT_CTL, MQTT_HOST, MQTT_PASS, MQTT_PORT, MQTT_USER

VOLUME_STEP = 10
VOLUME_DEFAULT = 100
VOL_BAR_WIDTH = 12


class TrackItem(ListItem):
    def __init__(self, path: Path) -> None:
        super().__init__()
        self.path = path
        self.display_name = path.stem.replace(".", " ")

    def compose(self) -> ComposeResult:
        yield Label(self.display_name)


class JukeboxApp(App):
    TITLE = "Jukebox"

    CSS = """
    #controls {
        width: 38;
        padding: 1 2;
        border-right: solid $surface-darken-2;
    }

    #np-label {
        color: $text-muted;
    }

    #np-divider {
        color: $surface-darken-2;
        margin-bottom: 1;
    }

    #np-title {
        text-style: bold;
        height: auto;
        margin-bottom: 0;
    }

    #np-state {
        height: 1;
        margin-bottom: 1;
    }

    #np-state.playing { color: $success; }
    #np-state.paused  { color: $warning; }

    #transport {
        height: auto;
        margin: 1 0;
    }

    Button {
        min-width: 5;
        margin-right: 1;
        height: 3;
    }

    #vol-display {
        margin-top: 1;
    }

    #shuffle-indicator {
        margin-top: 1;
        color: $text-muted;
    }

    #shuffle-indicator.on {
        color: $success;
    }

    ListView {
        width: 1fr;
    }

    TrackItem.playing Label {
        color: $success;
        text-style: bold;
    }
    """

    # Arrow keys (↑/↓) navigate the track list — handled natively by ListView.
    BINDINGS = [
        Binding("enter", "play",         "▶",  show=True),
        Binding("space", "pause_resume", "⏸",  show=True),
        Binding("w",     "skip",         "⏭",  show=True),
        Binding("e",     "stop",         "■",  show=True),
        Binding("s",     "shuffle",      "⇄",  show=True),
        Binding("y",     "vol_up",       "♪+", show=True),
        Binding("t",     "vol_down",     "♪−", show=True),
        Binding("q",     "quit",         "✕",  show=True),
        Binding("escape","quit",         "",   show=False),
    ]

    def __init__(self) -> None:
        super().__init__()
        self._volume   = VOLUME_DEFAULT
        self._paused   = False
        self._shuffled = False
        self._playing: TrackItem | None = None
        self._mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self._mqtt.username_pw_set(MQTT_USER, MQTT_PASS)

    def compose(self) -> ComposeResult:
        tracks = sorted(
            Path(MIDI_DIR).glob("*.mid"),
            key=lambda f: f.stat().st_mtime,
            reverse=True,
        )
        yield Header()
        with Horizontal():
            with Vertical(id="controls"):
                yield Static("♪  NOW PLAYING", id="np-label")
                yield Static("─" * 28,         id="np-divider")
                yield Static("—",              id="np-title")
                yield Static("",              id="np-state")
                with Horizontal(id="transport"):
                    yield Button("▶", id="btn-play",  variant="success")
                    yield Button("⏸", id="btn-pause", variant="default")
                    yield Button("⏭", id="btn-skip",  variant="default")
                    yield Button("■", id="btn-stop",  variant="error")
                yield Static(self._vol_text(),     id="vol-display")
                yield Static(self._shuffle_text(), id="shuffle-indicator")
            if tracks:
                yield ListView(*[TrackItem(f) for f in tracks])
            else:
                yield Static(f"No .mid files found in {MIDI_DIR}")
        yield Footer()

    def on_mount(self) -> None:
        try:
            self._mqtt.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            self._mqtt.loop_start()
        except Exception as exc:
            self.query_one("#np-state", Static).update(f"MQTT: {exc}")

    def on_unmount(self) -> None:
        self._mqtt.loop_stop()
        self._mqtt.disconnect()

    def _publish(self, message: str) -> None:
        self._mqtt.publish(MQTT_CTL, message)

    def _vol_text(self) -> str:
        filled = int(self._volume / 127 * VOL_BAR_WIDTH)
        bar = "█" * filled + "░" * (VOL_BAR_WIDTH - filled)
        return f"♪  [{bar}]  {self._volume}"

    def _shuffle_text(self) -> str:
        return "⇄  [ ON ]" if self._shuffled else "⇄  [ OFF ]"

    def _refresh(self) -> None:
        # track title
        self.query_one("#np-title", Static).update(
            self._playing.display_name if self._playing else "—"
        )

        # state dot
        state = self.query_one("#np-state", Static)
        if self._playing is None:
            state.update("")
            state.remove_class("playing", "paused")
        elif self._paused:
            state.update("● paused")
            state.remove_class("playing")
            state.add_class("paused")
        else:
            state.update("● playing")
            state.remove_class("paused")
            state.add_class("playing")

        # volume + shuffle
        self.query_one("#vol-display",      Static).update(self._vol_text())
        shuffle = self.query_one("#shuffle-indicator", Static)
        shuffle.update(self._shuffle_text())
        if self._shuffled:
            shuffle.add_class("on")
        else:
            shuffle.remove_class("on")

    # ── button clicks ──────────────────────────────────────────────────────────

    def on_button_pressed(self, event: Button.Pressed) -> None:
        dispatch = {
            "btn-play":  self.action_play,
            "btn-pause": self.action_pause_resume,
            "btn-skip":  self.action_skip,
            "btn-stop":  self.action_stop,
        }
        fn = dispatch.get(event.button.id)
        if fn:
            fn()

    # ── actions ────────────────────────────────────────────────────────────────

    def on_list_view_selected(self, _: ListView.Selected) -> None:
        self.action_play()

    def action_play(self) -> None:
        lv = self.query_one(ListView)
        item = lv.highlighted_child
        if not isinstance(item, TrackItem):
            return
        if self._playing:
            self._playing.remove_class("playing")
        self._playing = item
        self._playing.add_class("playing")
        self._paused = False
        self._publish(f"play/{item.path.name}")
        self._refresh()

    def action_pause_resume(self) -> None:
        if self._playing is None:
            return
        if self._paused:
            self._publish("resume")
            self._paused = False
        else:
            self._publish("pause")
            self._paused = True
        self._refresh()

    def action_stop(self) -> None:
        if self._playing is None:
            return
        self._publish("pause")
        self._paused = True
        self._refresh()

    def action_skip(self) -> None:
        self._publish("skip")
        if self._playing:
            self._playing.remove_class("playing")
            self._playing = None
        self._paused = False
        self._refresh()

    def action_shuffle(self) -> None:
        self._publish("shuffle")
        self._shuffled = not self._shuffled
        self._refresh()

    def action_vol_up(self) -> None:
        self._volume = min(127, self._volume + VOLUME_STEP)
        self._publish(f"volume/{self._volume}")
        self._refresh()

    def action_vol_down(self) -> None:
        self._volume = max(0, self._volume - VOLUME_STEP)
        self._publish(f"volume/{self._volume}")
        self._refresh()


if __name__ == "__main__":
    JukeboxApp().run()
