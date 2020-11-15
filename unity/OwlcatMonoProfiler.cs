using System.Collections;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Owlcat
{
    public class MonoProfiler : MonoBehaviour
    {
#if PROFILE_MONO
        [DllImport("mono_profiler_mono")]
        private static extern void StartProfiling();

        [DllImport("mono_profiler_mono")]
        private static extern void EndProfilingFrame();
#endif

        IEnumerator Start()
        {
#if PROFILE_MONO
            StartProfiling();
            yield return StartCoroutine("CallPluginAtEndOfFrames");
#endif
        }

#if PROFILE_MONO
        private IEnumerator CallPluginAtEndOfFrames()
        {
            while (true)
            {
                // Wait until all frame rendering is done
                yield return new WaitForEndOfFrame();

                EndProfilingFrame();
            }
        }
#endif
    }
}
