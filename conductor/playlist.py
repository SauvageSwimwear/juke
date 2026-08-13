import os
import random


def scan(directory) -> list[str]:
    """Return sorted list of .mid/.midi paths in directory."""
    paths = []
    for name in os.listdir(directory):
        if name.lower().endswith((".mid", ".midi")):
            paths.append(os.path.join(directory, name))
    return sorted(paths)


class Playlist:
    def __init__(self, directory):
        self._dir   = directory
        self._files = scan(directory)
        self._index = 0

    def current(self) -> str | None:
        if not self._files:
            return None
        return self._files[self._index]

    def advance(self):
        if not self._files:
            return
        self._index = (self._index + 1) % len(self._files)

    def select(self, filename) -> bool:
        """Jump to a specific filename. Returns False if not found."""
        target = os.path.basename(filename)
        for i, path in enumerate(self._files):
            if os.path.basename(path) == target:
                self._index = i
                return True
        return False

    def shuffle(self):
        current = self.current()
        random.shuffle(self._files)
        if current:
            # keep current at front so it finishes before shuffled order begins
            self._files.remove(current)
            self._files.insert(0, current)
            self._index = 0

    def __len__(self):
        return len(self._files)
