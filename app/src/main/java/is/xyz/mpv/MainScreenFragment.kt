package `is`.xyz.mpv

import `is`.xyz.mpv.preferences.PreferenceActivity
import `is`.xyz.mpv.databinding.FragmentMainScreenBinding
import android.content.Intent
import android.media.MediaExtractor
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.view.View
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import java.io.File
import java.io.FileOutputStream

class MainScreenFragment : Fragment(R.layout.fragment_main_screen) {
    private lateinit var binding: FragmentMainScreenBinding

    private lateinit var playerLauncher: ActivityResultLauncher<Intent>
    private lateinit var documentPicker: ActivityResultLauncher<Array<String>>

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        playerLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
            Log.v(TAG, "returned from player ($it)")
        }

        documentPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            uri?.let { validateAndPlay(it) }
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        binding = FragmentMainScreenBinding.bind(view)

        Utils.handleInsetsAsPadding(binding.root)

        // Demo video cards
        binding.cardOldTownCross.setOnClickListener { playDemoVideo("old_town_cross.mp4") }
        binding.cardCrowdRun.setOnClickListener { playDemoVideo("crowd_run.mp4") }
        binding.cardTractor.setOnClickListener { playDemoVideo("tractor.mp4") }
        binding.cardRiverbed.setOnClickListener { playDemoVideo("riverbed.mp4") }

        // Load custom video
        binding.loadCustomBtn.setOnClickListener {
            documentPicker.launch(arrayOf("video/*"))
        }

        // Settings
        binding.settingsBtn.setOnClickListener {
            startActivity(Intent(context, PreferenceActivity::class.java))
        }

        if (BuildConfig.DEBUG) {
            binding.settingsBtn.setOnLongClickListener { showDebugMenu(); true }
        }
    }

    /**
     * Copy a demo video from assets to internal storage (if needed) and launch the player.
     */
    private fun playDemoVideo(filename: String) {
        val ctx = requireContext()
        val demoDir = File(ctx.filesDir, "anvil/demo")
        demoDir.mkdirs()
        val destFile = File(demoDir, filename)

        // Copy from assets if not already present
        if (!destFile.exists() || destFile.length() == 0L) {
            try {
                ctx.assets.open("anvil/demo/$filename").use { input ->
                    FileOutputStream(destFile).use { output ->
                        input.copyTo(output)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Failed to copy demo asset $filename", e)
                return
            }
        }

        playFile(destFile.absolutePath)
    }

    /**
     * Validate that a user-picked video is H.264 1080p before launching.
     */
    private fun validateAndPlay(uri: Uri) {
        val ctx = requireContext()
        var codec = "unknown"
        var width = 0
        var height = 0

        try {
            val extractor = MediaExtractor()
            extractor.setDataSource(ctx, uri, null)
            for (i in 0 until extractor.trackCount) {
                val format = extractor.getTrackFormat(i)
                val mime = format.getString(android.media.MediaFormat.KEY_MIME) ?: continue
                if (mime.startsWith("video/")) {
                    codec = mime
                    if (format.containsKey(android.media.MediaFormat.KEY_WIDTH))
                        width = format.getInteger(android.media.MediaFormat.KEY_WIDTH)
                    if (format.containsKey(android.media.MediaFormat.KEY_HEIGHT))
                        height = format.getInteger(android.media.MediaFormat.KEY_HEIGHT)
                    break
                }
            }
            extractor.release()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to probe video", e)
        }

        val isH264 = codec == "video/avc"
        val is1080p = (width == 1920 && height == 1080) || (width == 1080 && height == 1920)

        if (!isH264 || !is1080p) {
            val codecDisplay = when (codec) {
                "video/avc" -> "H.264"
                "video/hevc" -> "H.265"
                "video/x-vnd.on2.vp9" -> "VP9"
                "video/av01" -> "AV1"
                else -> codec
            }
            AlertDialog.Builder(ctx)
                .setTitle(R.string.demo_invalid_title)
                .setMessage(getString(R.string.demo_invalid_msg, codecDisplay, width, height))
                .setPositiveButton(R.string.dialog_ok, null)
                .show()
            return
        }

        // Launch via ACTION_VIEW with the content URI
        val intent = Intent(Intent.ACTION_VIEW, uri)
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        intent.setClass(ctx, MPVActivity::class.java)
        playerLauncher.launch(intent)
    }

    private fun playFile(filepath: String) {
        val intent = Intent()
        intent.putExtra("filepath", filepath)
        intent.setClass(requireContext(), MPVActivity::class.java)
        playerLauncher.launch(intent)
    }

    private fun showDebugMenu() {
        assert(BuildConfig.DEBUG)
        val context = requireContext()
        with(AlertDialog.Builder(context)) {
            setItems(DEBUG_ACTIVITIES) { dialog, idx ->
                dialog.dismiss()
                val intent = Intent(Intent.ACTION_MAIN)
                intent.setClassName(context, "${context.packageName}.${DEBUG_ACTIVITIES[idx]}")
                startActivity(intent)
            }
            create().show()
        }
    }

    companion object {
        private const val TAG = "mpv"

        private val DEBUG_ACTIVITIES = arrayOf(
            "IntentTestActivity",
            "CodecInfoActivity"
        )
    }
}
