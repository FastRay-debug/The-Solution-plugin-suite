using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Windows.Forms;

class SuiteSetup {
    [STAThread]
    static void Main() {
        Application.EnableVisualStyles();
        var res = MessageBox.Show(
            "Install The Solution Suite (13 Modules: VST3, CLAP, AAX, Standalone)?\n\nPlugins will be installed to Common Files and Program Files\\The Solution.",
            "The Solution Suite Setup",
            MessageBoxButtons.OKCancel,
            MessageBoxIcon.Information
        );
        if (res != DialogResult.OK) return;

        string tmpDir = Path.Combine(Path.GetTempPath(), "TheSolutionSetupTmp");
        if (Directory.Exists(tmpDir)) Directory.Delete(tmpDir, true);
        Directory.CreateDirectory(tmpDir);
        string tmpZip = Path.Combine(tmpDir, "suite_payload.zip");
        using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("suite_payload.zip"))
        using (FileStream fs = File.Create(tmpZip))
            s.CopyTo(fs);
        ZipFile.ExtractToDirectory(tmpZip, tmpDir);

        string cf = Environment.GetFolderPath(Environment.SpecialFolder.CommonProgramFiles);
        string pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        CopyDir(Path.Combine(tmpDir, "VST3"), Path.Combine(cf, "VST3", "The Solution"));
        CopyDir(Path.Combine(tmpDir, "CLAP"), Path.Combine(cf, "CLAP", "The Solution"));
        CopyDir(Path.Combine(tmpDir, "AAX"), Path.Combine(cf, "Avid", "Audio", "Plug-Ins", "The Solution"));
        CopyDir(Path.Combine(tmpDir, "Standalone"), Path.Combine(pf, "The Solution", "Standalone"));

        Directory.Delete(tmpDir, true);
        MessageBox.Show("The Solution Suite installed cleanly!", "Setup Complete", MessageBoxButtons.OK, MessageBoxIcon.Information);
    }

    static void CopyDir(string src, string dst) {
        if (!Directory.Exists(src)) return;
        Directory.CreateDirectory(dst);
        foreach (string f in Directory.GetFiles(src)) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), true);
        foreach (string d in Directory.GetDirectories(src)) CopyDir(d, Path.Combine(dst, Path.GetFileName(d)));
    }
}
