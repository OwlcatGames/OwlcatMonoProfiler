using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

public class ProfilerStarter : MonoBehaviour
{
    [DllImport("mono_profiler_mono")]
    extern static void StartProfiling();

    [DllImport("mono_profiler_mono")]
    extern static void EndProfilingFrame();

    public GameObject Indicator;

    private List<System.Object> m_Test = new List<System.Object>();
    private int m_Frame = 0;

    class MyTest
    {
        int a = 800;
    }

    // Start is called before the first frame update
    void Start()
    {
        StartProfiling();
        Indicator.SetActive(true);
    }

    // Update is called once per frame
    void Update()
    {
        ++m_Frame;
        if (m_Frame % (60 * 30) == 0)
        {
            m_Test.Clear();
            GC.Collect();
        }
        else
        {
            for (int i = 0; i < 1000; ++i)
            {
                int r = UnityEngine.Random.Range(0, 4);
                switch (r)
                {
                    case 0: m_Test.Add(new String('a', 10)); break;
                    case 1: m_Test.Add(new Single[] { 1, 2, 3 }); break;
                    //case 2: m_Test.Add(GameObject.CreatePrimitive(PrimitiveType.Cube)); break;
                    case 3: m_Test.Add(new MyTest()); break;
                }
            }
        }

        EndProfilingFrame();
    }
}
