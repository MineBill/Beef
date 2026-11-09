using System;
using System.Collections;
using Beefy;
using Beefy.widgets;
using Beefy.events;
using Beefy.theme.dark;
using Beefy.gfx;
using Beefy.geom;

namespace IDE.ui
{
	// A VS Code-style command palette: a search box over a filtered list of every IDE command. Opened with
	// Ctrl+Shift+P ("Show Command Palette"). Type to filter, Up/Down to move, Enter to run, Escape to close.
	class CommandPalette : PaletteWidget
	{
		public static void Show()
		{
			if (gApp.mCommandPalette != null)
			{
				// Toggle off if it's already open.
				gApp.mCommandPalette.Close();
				return;
			}

			var palette = new CommandPalette();
			palette.Init();
		}

		void Init()
		{
			gApp.mCommandPalette = this;
			// The owning window deletes us on close; just drop the app reference then.
			mOnRemovedFromParent.Add(new (widget, prevParent, widgetWindow) =>
				{
					if (gApp.mCommandPalette == this)
						gApp.mCommandPalette = null;
				});

			InitPalette("Command Palette");
		}

		protected override void RebuildEntries(StringView queryView)
		{
			String query = scope String(queryView);

			for (var cmd in gApp.mCommands.mCommandMap.Values)
			{
				String name = cmd.mName;

				var entry = new Entry();
				entry.mName = new String(name);
				String shortcut = scope String();
				cmd.ToString(shortcut);
				entry.mDetail = new String(shortcut);

				if (!query.IsEmpty)
				{
					int32 score = 0;
					if (!FuzzyMatch(query, name, out score))
					{
						delete entry;
						continue;
					}
					entry.mScore = score;
				}

				mEntries.Add(entry);
			}
		}

		protected override void RunEntry(Entry entry)
		{
			String name = scope String(entry.mName);
			IDECommand cmd;
			if (gApp.mCommands.mCommandMap.TryGetValue(name, out cmd))
				cmd.mAction();
		}
	}
}
