// Kid Pix Deluxe 3 launcher: pick a window size, then start the game.
//
// The size is applied by rewriting a few keys in _Play\ddraw.ini (cnc-ddraw's
// config) before launching KPD3.exe. Everything about a size lives in the
// Presets table below.
//
// Command line (no window shown):
//   "Kid Pix Launcher.exe" --size original|large|xlarge|fullscreen   apply and launch
//   "Kid Pix Launcher.exe" --apply <size>                             apply only
//
// Built with the .NET Framework compiler that ships with Windows (C# 5).

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using Microsoft.Win32;

namespace KidPixLauncher
{
    /* How cnc-ddraw stretches the game's 800x600 picture into the window. */
    enum Scaling
    {
        Exact,        /* 1:1, nothing to stretch */
        WholePixels,  /* integer multiples only (boxing), every pixel the same size */
        Fit           /* largest size that keeps 4:3 (maintas) */
    }

    class Preset
    {
        public readonly string Id, Title, Detail;
        public readonly int Width, Height;     // ignored when Fullscreen
        public readonly bool Fullscreen;
        public readonly Scaling Scaling;

        public Preset(string id, string title, string detail, int width, int height, bool fullscreen, Scaling scaling)
        {
            Id = id; Title = title; Detail = detail;
            Width = width; Height = height; Fullscreen = fullscreen; Scaling = scaling;
        }
    }

    static class Game
    {
        public static readonly Preset[] Presets =
        {
            new Preset("original",   "Original",    "800 × 600 — as it shipped",          800,  600, false, Scaling.Exact),
            new Preset("large",      "Large",       "1200 × 900 — 1.5×",                 1200,  900, false, Scaling.Fit),
            new Preset("xlarge",     "Extra large", "1600 × 1200 — 2×",                  1600, 1200, false, Scaling.WholePixels),
            new Preset("fullscreen", "Fullscreen",  "Fills the screen, keeps the shape",    0,    0, true,  Scaling.Fit),
        };

        public static string PlayDir;
        public static string Exe { get { return Path.Combine(PlayDir, "KPD3.exe"); } }
        static string DdrawIni { get { return Path.Combine(PlayDir, "ddraw.ini"); } }

        const string SettingsKey = @"Software\KidPixDeluxe3\Launcher";

        /* The launcher sits next to the _Play folder, or inside it. */
        public static bool Locate()
        {
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            foreach (string dir in new[] { Path.Combine(baseDir, "_Play"), baseDir })
            {
                if (File.Exists(Path.Combine(dir, "KPD3.exe"))) { PlayDir = dir; return true; }
            }
            return false;
        }

        public static Preset Find(string id)
        {
            return Presets.FirstOrDefault(p => string.Equals(p.Id, id, StringComparison.OrdinalIgnoreCase));
        }

        /* A windowed size is offered only if it fits the screen with its frame. */
        public static bool Fits(Preset p)
        {
            if (p.Fullscreen) return true;
            Rectangle work = Screen.PrimaryScreen.WorkingArea;
            return p.Width + 16 <= work.Width && p.Height + 40 <= work.Height;
        }

        public static void ApplySize(Preset p)
        {
            var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            values["fullscreen"] = p.Fullscreen ? "true" : "false";
            values["windowed"]   = "true";                       /* borderless when fullscreen */
            values["border"]     = p.Fullscreen ? "false" : "true";
            values["width"]      = p.Fullscreen ? "0" : p.Width.ToString();
            values["height"]     = p.Fullscreen ? "0" : p.Height.ToString();
            values["maintas"]    = p.Scaling == Scaling.Fit ? "true" : "false";
            values["boxing"]     = p.Scaling == Scaling.WholePixels ? "true" : "false";
            /* Kept here so the ini cannot drift from what the game depends on:
               the GDI renderer is the tested one, and fixchilds=0 stops cnc-ddraw
               dropping to unscaled output if the game ever creates a child window. */
            values["renderer"]   = "gdi";
            values["fixchilds"]  = "0";
            values["singlecpu"]  = "true";   /* else cnc-ddraw undoes the shim's one-CPU fix for the game's race */
            SetIniValues(DdrawIni, "ddraw", values);
        }

        /* Rewrites key=value lines inside [section], appending keys it did not find.
           Comments, ordering and every other key are left exactly as they were. */
        static void SetIniValues(string path, string section, Dictionary<string, string> values)
        {
            var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : new List<string>();
            var pending = new Dictionary<string, string>(values, StringComparer.OrdinalIgnoreCase);
            int start = -1, end = lines.Count;

            for (int i = 0; i < lines.Count; i++)
            {
                string t = lines[i].Trim();
                if (t.StartsWith("["))
                {
                    if (start >= 0) { end = i; break; }
                    if (string.Equals(t, "[" + section + "]", StringComparison.OrdinalIgnoreCase)) start = i;
                    continue;
                }
                if (start < 0 || t.StartsWith(";") || t.StartsWith("#")) continue;
                int eq = t.IndexOf('=');
                if (eq <= 0) continue;
                string key = t.Substring(0, eq).Trim();
                string value;
                if (pending.TryGetValue(key, out value))
                {
                    lines[i] = key + "=" + value;
                    pending.Remove(key);
                }
            }

            if (start < 0)
            {
                lines.Add("[" + section + "]");
                start = lines.Count - 1;
                end = lines.Count;
            }
            int at = end;
            while (at - 1 > start && lines[at - 1].Trim().Length == 0) at--;
            foreach (var kv in pending) lines.Insert(at++, kv.Key + "=" + kv.Value);

            File.WriteAllText(path, string.Join("\r\n", lines) + "\r\n", new UTF8Encoding(false));
        }

        public static bool IsRunning()
        {
            return Process.GetProcessesByName("KPD3").Length > 0;
        }

        /* After a crash, Windows' Program Compatibility Assistant records the game and
           may add its own fixes on the next launch. Dropping its record avoids that.
           (Windows' built-in entry for Kid Pix Deluxe 3, whose Windows 98 mode asks for
           administrator rights, is avoided by the manual being named KPD3Manual.pdf.) */
        static void ClearCompatAssistantRecord()
        {
            try
            {
                using (RegistryKey k = Registry.CurrentUser.OpenSubKey(
                    @"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Compatibility Assistant\Store", true))
                {
                    if (k != null && k.GetValue(Exe) != null) k.DeleteValue(Exe, false);
                }
            }
            catch { }
        }

        /* The Brazilian Portuguese edition checks for its CD at every start: KPD3.exe
           opens the file named by the first FileCheck= line of kprun.ini (in its
           working folder) and quits unless it exists. Its own installer wrote the CD
           drive into kprun.ini; do the same before each start, so the CD, or a mounted
           copy of it, can be in any drive. The US edition has no kprun.ini. */
        static bool PointKprunAtDisc()
        {
            string ini = Path.Combine(PlayDir, "kprun.ini");
            if (!File.Exists(ini)) return true;
            string drive = null;
            foreach (DriveInfo d in DriveInfo.GetDrives())
            {
                try
                {
                    if (d.DriveType == DriveType.CDRom && d.IsReady &&
                        File.Exists(Path.Combine(d.RootDirectory.FullName, @"INSTALL\KP\378180.CD")))
                    {
                        drive = d.RootDirectory.FullName.Substring(0, 2);
                        break;
                    }
                }
                catch { }
            }
            if (drive == null)
            {
                MessageBox.Show("Insira o CD do Kid Pix Deluxe 3 (ou abra a imagem ISO do CD) e tente de novo.\n\n" +
                                "Insert the Kid Pix Deluxe 3 CD (or mount the CD's ISO image) and try again.",
                                "Kid Pix Deluxe 3", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return false;
            }
            Encoding enc = Encoding.Default;
            string[] lines = File.ReadAllLines(ini, enc);
            for (int i = 0; i < lines.Length; i++)
            {
                int eq = lines[i].IndexOf('=');
                if (eq > 0 && lines[i].Length > eq + 3 && lines[i][eq + 2] == ':' && lines[i][eq + 3] == '\\' &&
                    char.IsLetter(lines[i][eq + 1]))
                    lines[i] = lines[i].Substring(0, eq + 1) + drive + lines[i].Substring(eq + 3);
            }
            File.WriteAllText(ini, string.Join("\r\n", lines) + "\r\n", enc);
            return true;
        }

        public static bool Launch()
        {
            if (!PointKprunAtDisc()) return false;
            ClearCompatAssistantRecord();
            var psi = new ProcessStartInfo(Exe);
            psi.WorkingDirectory = PlayDir;
            psi.UseShellExecute = true;
            Process.Start(psi);
            return true;
        }

        public static string LastChoice()
        {
            try
            {
                using (RegistryKey k = Registry.CurrentUser.OpenSubKey(SettingsKey))
                    return k == null ? null : k.GetValue("Size") as string;
            }
            catch { return null; }
        }

        public static void SaveChoice(Preset p)
        {
            try
            {
                using (RegistryKey k = Registry.CurrentUser.CreateSubKey(SettingsKey))
                    k.SetValue("Size", p.Id);
            }
            catch { }
        }
    }

    /* One selectable size, drawn as a card. */
    class SizeCard : Control
    {
        public readonly Preset Preset;
        public readonly bool Available;
        bool selected, hover;

        public event EventHandler Chosen;     /* single click */
        public event EventHandler Activated;  /* double click */

        static readonly Color Accent      = Color.FromArgb(109, 40, 217);
        static readonly Color AccentLight = Color.FromArgb(237, 233, 254);
        static readonly Color Line        = Color.FromArgb(221, 221, 230);
        static readonly Color Muted       = Color.FromArgb(100, 100, 115);

        public SizeCard(Preset preset, bool available)
        {
            Preset = preset;
            Available = available;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.UserPaint | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
            Cursor = available ? Cursors.Hand : Cursors.Default;
            TabStop = available;
        }

        public bool Selected
        {
            get { return selected; }
            set { if (selected != value) { selected = value; Invalidate(); } }
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }

        protected override void OnMouseClick(MouseEventArgs e)
        {
            base.OnMouseClick(e);
            if (Available && Chosen != null) Chosen(this, EventArgs.Empty);
        }

        protected override void OnMouseDoubleClick(MouseEventArgs e)
        {
            base.OnMouseDoubleClick(e);
            if (Available && Activated != null) Activated(this, EventArgs.Empty);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            g.Clear(Parent != null ? Parent.BackColor : Color.White);

            int pad = Height / 5;
            var r = new Rectangle(1, 1, Width - 3, Height - 3);
            Color back = !Available ? Color.FromArgb(246, 246, 248)
                       : selected   ? AccentLight
                       : hover      ? Color.FromArgb(249, 248, 255)
                       :              Color.White;

            using (GraphicsPath path = Rounded(r, Height / 6))
            using (var fill = new SolidBrush(back))
            using (var pen = new Pen(selected ? Accent : Line, selected ? 2f : 1f))
            {
                g.FillPath(fill, path);
                g.DrawPath(pen, path);
            }

            /* radio mark */
            int d = Height / 3;
            var dot = new Rectangle(pad, (Height - d) / 2, d, d);
            using (var pen = new Pen(Available ? (selected ? Accent : Muted) : Line, 2f))
                g.DrawEllipse(pen, dot);
            if (selected)
            {
                var inner = Rectangle.Inflate(dot, -d / 4, -d / 4);
                using (var b = new SolidBrush(Accent)) g.FillEllipse(b, inner);
            }

            int textX = dot.Right + pad;
            using (var titleFont = new Font(Font.FontFamily, Font.Size * 1.1f, FontStyle.Bold))
            using (var titleBrush = new SolidBrush(Available ? Color.FromArgb(30, 27, 46) : Muted))
            using (var detailBrush = new SolidBrush(Muted))
            {
                string detail = Available ? Preset.Detail : Preset.Detail + "  (too big for this screen)";
                SizeF ts = g.MeasureString(Preset.Title, titleFont);
                SizeF ds = g.MeasureString(detail, Font);
                float top = (Height - ts.Height - ds.Height) / 2f;
                g.DrawString(Preset.Title, titleFont, titleBrush, textX, top);
                g.DrawString(detail, Font, detailBrush, textX, top + ts.Height);
            }

            /* dotted focus box only once the keyboard is in use */
            if (Focused && ShowFocusCues && Available)
                ControlPaint.DrawFocusRectangle(g, Rectangle.Inflate(r, -4, -4));
        }

        static GraphicsPath Rounded(Rectangle r, int radius)
        {
            int d = Math.Max(2, radius * 2);
            var p = new GraphicsPath();
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }
    }

    class LauncherForm : Form
    {
        readonly List<SizeCard> cards = new List<SizeCard>();
        readonly Label status;
        SizeCard current;

        public LauncherForm()
        {
            Text = "Kid Pix Deluxe 3";
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.White;
            Font = new Font("Segoe UI", 9.75f);
            KeyPreview = true;

            /* Fonts are in points and follow the screen DPI on their own; pixel
               layout does not, and WinForms' AutoScale did not apply when built
               in code. Scale every layout number explicitly instead. */
            AutoScaleMode = AutoScaleMode.None;
            float k;
            using (Graphics g = CreateGraphics()) k = g.DpiX / 96f;
            Func<int, int> S = v => (int)Math.Round(v * k);

            const int W = 380, margin = 18, cardH = 64, gap = 8;
            int inner = W - 2 * margin;

            Icon gameIcon = null;
            try { gameIcon = Icon.ExtractAssociatedIcon(Game.Exe); } catch { }
            if (gameIcon != null) Icon = gameIcon;

            var header = new Panel { Dock = DockStyle.Top, Height = S(84), BackColor = Color.FromArgb(76, 29, 149) };
            var logo = new PictureBox
            {
                Image = (gameIcon ?? SystemIcons.Application).ToBitmap(),
                SizeMode = PictureBoxSizeMode.Zoom,
                Location = new Point(S(margin), S(24)), Size = new Size(S(36), S(36)), BackColor = Color.Transparent,
            };
            var title = new Label
            {
                Text = "Kid Pix Deluxe 3", AutoSize = true, ForeColor = Color.White, BackColor = Color.Transparent,
                Font = new Font("Segoe UI Semibold", 15f), Location = new Point(S(64), S(14)),
            };
            var subtitle = new Label
            {
                Text = "Choose a window size", AutoSize = true, BackColor = Color.Transparent,
                ForeColor = Color.FromArgb(221, 214, 254), Location = new Point(S(66), S(50)),
            };
            header.Controls.AddRange(new Control[] { logo, title, subtitle });
            Controls.Add(header);

            int y = 84 + 16;
            foreach (Preset p in Game.Presets)
            {
                var card = new SizeCard(p, Game.Fits(p))
                {
                    Location = new Point(S(margin), S(y)), Size = new Size(S(inner), S(cardH)),
                };
                card.Chosen += (s, e) => Choose((SizeCard)s);
                card.Activated += (s, e) => { Choose((SizeCard)s); Play(); };
                card.GotFocus += (s, e) => ((Control)s).Invalidate();
                card.LostFocus += (s, e) => ((Control)s).Invalidate();
                cards.Add(card);
                Controls.Add(card);
                y += cardH + gap;
            }

            var play = new Button
            {
                Text = "Play", Location = new Point(S(margin), S(y + 8)), Size = new Size(S(inner), S(48)),
                FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(109, 40, 217), ForeColor = Color.White,
                Font = new Font("Segoe UI Semibold", 12f), Cursor = Cursors.Hand,
            };
            play.FlatAppearance.BorderSize = 0;
            play.FlatAppearance.MouseOverBackColor = Color.FromArgb(91, 33, 182);
            play.Click += (s, e) => Play();
            Controls.Add(play);
            AcceptButton = play;
            y += 8 + 48 + 8;

            status = new Label
            {
                AutoSize = false, Location = new Point(S(margin), S(y)), Size = new Size(S(inner), S(22)),
                ForeColor = Color.FromArgb(185, 28, 28), TextAlign = ContentAlignment.MiddleCenter,
            };
            Controls.Add(status);
            y += 22 + 10;

            ClientSize = new Size(S(W), S(y));

            /* last choice if it still fits; on first run the biggest window that fits */
            Preset last = Game.Find(Game.LastChoice());
            SizeCard initial = cards.FirstOrDefault(c => c.Preset == last && c.Available)
                            ?? cards.LastOrDefault(c => c.Available && !c.Preset.Fullscreen)
                            ?? cards[0];
            Choose(initial);
        }

        void Choose(SizeCard card)
        {
            current = card;
            foreach (SizeCard c in cards) c.Selected = c == card;
            status.Text = "";
        }

        /* arrow keys move between the sizes that fit */
        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            if (keyData == Keys.Up || keyData == Keys.Down)
            {
                var usable = cards.Where(c => c.Available).ToList();
                int i = usable.IndexOf(current) + (keyData == Keys.Down ? 1 : -1);
                if (i >= 0 && i < usable.Count) { Choose(usable[i]); usable[i].Focus(); }
                return true;
            }
            return base.ProcessCmdKey(ref msg, keyData);
        }

        void Play()
        {
            if (current == null) return;
            if (Game.IsRunning())
            {
                status.Text = "Kid Pix is already running — close it to change the size.";
                return;
            }
            try
            {
                Game.ApplySize(current.Preset);
                Game.SaveChoice(current.Preset);
                if (Game.Launch()) Close();
            }
            catch (Exception ex)
            {
                status.Text = "Could not start Kid Pix: " + ex.Message;
            }
        }
    }

    static class Program
    {
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

        [STAThread]
        static int Main(string[] args)
        {
            try { SetProcessDPIAware(); } catch { }   /* crisp text at 150% scaling */

            if (!Game.Locate())
            {
                MessageBox.Show("Couldn't find KPD3.exe.\n\nPut this launcher next to the _Play folder.",
                                "Kid Pix Deluxe 3", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }

            /* --size <id> applies and launches; --apply <id> only applies */
            if (args.Length == 2 && (args[0] == "--size" || args[0] == "--apply"))
            {
                Preset p = Game.Find(args[1]);
                if (p == null)
                {
                    MessageBox.Show("Unknown size '" + args[1] + "'. Use: " +
                                    string.Join(", ", Game.Presets.Select(x => x.Id).ToArray()),
                                    "Kid Pix Deluxe 3", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return 2;
                }
                Game.ApplySize(p);
                if (args[0] == "--size")
                {
                    Game.SaveChoice(p);
                    if (!Game.Launch()) return 3;
                }
                return 0;
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new LauncherForm());
            return 0;
        }
    }
}
