#!/usr/bin/env python3
"""Add post-upload checksum checks to the selected SDK's existing launch flow."""
import importlib.util
from pathlib import Path
import sys


def verify_uploads(sdk):
    class VerifiedUploads(sdk.FlipperStorageOperations):
        def send_file_to_storage(self, target, source, force=False):
            super().send_file_to_storage(target, source, force)
            if self.storage.hash_local(source) != self.storage.hash_flipper(target):
                raise ValueError(f'Uploaded file checksum mismatch: {target}')

    # The SDK runs every send before its launch operation and catches upload failures.
    sdk.FlipperStorageOperations = VerifiedUploads


def main():
    uploader = Path(sys.argv.pop(1))
    sys.path.insert(0, str(uploader.parent))
    spec = importlib.util.spec_from_file_location('selected_sdk_runfap', uploader)
    sdk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sdk)
    verify_uploads(sdk)
    sdk.Main()()


if __name__ == '__main__':
    main()
