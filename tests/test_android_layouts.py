#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check companion XML layout contracts without an Android SDK or device.

This checks resource structure/bindings, not Android inflation or UI interaction.
"""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


APP = Path(__file__).resolve().parents[1] / "android/deskflow-otg/app/src/main"
ANDROID = "{http://schemas.android.com/apk/res/android}"
LAYOUTS = {
    "activity_main": {
        "connection_status": "TextView",
        "notification_status": "TextView",
        "notification_settings": "Button",
        "autoplay_enabled": "Switch",
        "autoplay_target": "TextView",
        "autoplay_select_player": "Button",
        "open_transfers": "Button",
    },
    "activity_transfer": {
        "transfer_status": "TextView",
        "transfer_refresh": "Button",
        "transfer_rows": "LinearLayout",
    },
    "activity_share": {
        "share_status": "TextView",
        "share_progress": "ProgressBar",
        "share_confirm": "Button",
        "share_cancel": "Button",
    },
    "item_transfer": {
        "transfer_label": "TextView",
        "transfer_save": "Button",
        "transfer_delete": "Button",
    },
    "transfer_empty": {},
}


class AndroidLayoutsTest(unittest.TestCase):
    def setUp(self):
        self.layouts = {name: ET.parse(APP / f"res/layout/{name}.xml").getroot()
                        for name in LAYOUTS}

    def view(self, layout, name):
        return next(view for view in self.layouts[layout].iter()
                    if view.get(ANDROID + "id") == f"@+id/{name}")

    def test_view_ids_types_and_dimensions(self):
        for name, root in self.layouts.items():
            ids = {}
            for view in root.iter():
                with self.subTest(layout=name, view=view.tag):
                    self.assertIsNotNone(view.get(ANDROID + "layout_width"))
                    self.assertIsNotNone(view.get(ANDROID + "layout_height"))
                    identifier = view.get(ANDROID + "id")
                    if identifier:
                        self.assertTrue(identifier.startswith("@+id/"))
                        self.assertNotIn(identifier[5:], ids)
                        ids[identifier[5:]] = view.tag
                    if view.tag == "LinearLayout":
                        self.assertEqual(view.get(ANDROID + "orientation"), "vertical")
                    if view.tag == "ScrollView":
                        self.assertEqual(len(view), 1)
            self.assertEqual(ids, LAYOUTS[name])

    def test_activities_load_layouts_and_bind_existing_views(self):
        for activity, layout_names in (
                ("MainActivity", ("activity_main",)),
                ("TransferActivity", ("activity_transfer", "item_transfer", "transfer_empty")),
                ("ShareActivity", ("activity_share",))):
            source = (APP / f"java/com/github/gh6497/deskflowotg/{activity}.java").read_text()
            self.assertIn(f"setContentView(R.layout.{layout_names[0]})", source)
            self.assertEqual(set(re.findall(r"R\.layout\.(\w+)", source)), set(layout_names))
            expected_ids = {name for layout in layout_names for name in LAYOUTS[layout]}
            self.assertEqual(set(re.findall(r"findViewById\(R\.id\.(\w+)\)", source)), expected_ids)
            self.assertNotRegex(source, r"new (?:TextView|Button|Switch|ProgressBar|LinearLayout|ScrollView)\(")

    def test_initial_states(self):
        self.assertEqual(self.layouts["activity_main"].tag, "ScrollView")
        self.assertEqual(self.view("activity_main", "connection_status").get(ANDROID + "text"),
                         "@string/connection_unknown")
        self.assertNotEqual(self.view("activity_main", "autoplay_enabled").get(ANDROID + "checked"), "true")
        self.assertEqual(self.view("item_transfer", "transfer_save").get(ANDROID + "visibility"), "gone")
        progress = self.view("activity_share", "share_progress")
        self.assertEqual(progress.get("style"), "?android:attr/progressBarStyleHorizontal")
        self.assertEqual(progress.get(ANDROID + "max"), "100")
        self.assertEqual(self.layouts["transfer_empty"].get(ANDROID + "text"), "@string/transfer_empty")

    def test_layout_text_is_localized(self):
        resources = {language: {value.get("name"): value for value in
                               ET.parse(APP / f"res/{language}/strings.xml").getroot()}
                     for language in ("values", "values-zh-rCN")}
        for root in self.layouts.values():
            for view in root.iter():
                text = view.get(ANDROID + "text")
                if text is None or text.startswith("@android:string/"):
                    continue
                self.assertTrue(text.startswith("@string/"), text)
                key = text[len("@string/"):]
                self.assertIn(key, resources["values"])
                if resources["values"][key].get("translatable") != "false":
                    self.assertIn(key, resources["values-zh-rCN"])

    def test_autoplay_switch_touch_target(self):
        switch = self.view("activity_main", "autoplay_enabled")
        # The whole row is clickable; its height must also meet the 48dp minimum.
        self.assertEqual(switch.get(ANDROID + "layout_width"), "match_parent")
        minimum = switch.get(ANDROID + "minHeight", "0dp")
        self.assertTrue(minimum.endswith("dp"))
        self.assertGreaterEqual(float(minimum[:-2]), 48)

    def test_theme_uses_a_known_system_font_family(self):
        resources = ET.parse(APP / "res/values/styles.xml").getroot()
        theme = next(style for style in resources.findall("style")
                     if style.get("name") == "Theme.DeskflowOtg")
        family = next(item for item in theme.findall("item")
                      if item.get("name") == "android:fontFamily")
        # Layoutlib interprets unknown names (including "sans") as font paths.
        # A missing asset then fails in Font_Builder_Delegate during inflation.
        self.assertEqual(family.text, "sans-serif")


if __name__ == "__main__":
    unittest.main()
