// Monaco editor integration for Verilog/VHDL
import { useRef } from 'react';
import Editor, { OnMount } from '@monaco-editor/react';

interface CodeEditorProps {
  value: string;
  onChange?: (value: string) => void;
  language?: string;
  height?: string | number;
}

export function CodeEditor({
  value, onChange, language = 'verilog', height = '200px',
}: CodeEditorProps) {
  const editorRef = useRef<any>(null);

  const onMount: OnMount = (editor) => {
    editorRef.current = editor;
  };

  return (
    <Editor
      height={height}
      language={language === 'verilog' ? 'systemverilog' : language === 'vhdl' ? 'vhdl' : 'verilog'}
      value={value}
      onChange={(v) => onChange?.(v ?? '')}
      onMount={onMount}
      theme="vs-dark"
      loading={<div style={{color:'#888',padding:8}}>Loading editor...</div>}
      options={{
        minimap: { enabled: false },
        fontSize: 13,
        lineNumbers: 'on',
        scrollBeyondLastLine: false,
        tabSize: 2,
      }}
    />
  );
}
