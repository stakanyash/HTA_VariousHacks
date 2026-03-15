#pragma once

namespace m3d
{
    struct sArg
    {
        enum eArgType
        {
            ARGTYPE_VOID = 0,
            ARGTYPE_INT = 1,
            ARGTYPE_FLOAT = 2,
            ARGTYPE_BOOL = 3,
            ARGTYPE_STRING = 4,
            ARGTYPE_VECTOR = 5,
            ARGTYPE_OBJECT = 6,
            ARGTYPE_QUATERNION = 7,
        };

        eArgType m_type = ARGTYPE_VOID;
        union
        {
            int   m_i = 0;
            float m_f;
            bool  m_b;
            char* m_s;
            void* m_o;
            float m_v[3];
            float m_q[4];
        };
    };
    static_assert(sizeof(sArg) == 0x0014);

    struct sArgStack
    {
        sArg         m_InArgs[16];
        sArg         m_OutArgs[16];
        unsigned int m_numInArgs = 0;
        unsigned int m_numOutArgs = 0;
        unsigned int m_curInArg = 0;
        unsigned int m_curOutArg = 0;

        void clear()
        {
            m_numInArgs = m_numOutArgs = m_curInArg = m_curOutArg = 0;
        }

        sArg* newIn() { return &m_InArgs[m_numInArgs++]; }
        sArg* newOut() { return &m_OutArgs[m_numOutArgs++]; }
    };
    static_assert(sizeof(sArgStack) == 0x0290);
}
