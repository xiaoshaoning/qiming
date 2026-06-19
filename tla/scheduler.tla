---- MODULE scheduler ----
(*
  TLA+ specification of the Qiming delta-cycle scheduler.

  The scheduler manages simulation events at (time, delta) pairs.
  Events at the same time are stratified by delta cycle.
  When no events remain at the current delta, delta resets and
  time advances to the next scheduled event time.

  Extended with models of:
  - Phase 1.5 port wire propagation (INPUT-only + cascade loop)
  - Phase 2 process triggering on signal changes
  - Phase 3 backward propagation
  - Immediate write for VHDL CSA (delay>0)
  - No stale-event overwrite property
*)

EXTENDS Naturals, Sequences, TLC

CONSTANTS
    MaxDelta,           (* maximum delta cycles before timeout *)
    NumSignals,         (* number of signals *)
    InputPort,          (* signal index acting as input port parent *)
    OutputPort,         (* signal index acting as output port child *)

ASSUME MaxDelta \in Nat \ {0}
ASSUME NumSignals > 1
ASSUME InputPort < NumSignals
ASSUME OutputPort < NumSignals

(* --algorithm scheduler

variables
    time = 0,
    delta = 0,
    (* Queue of scheduled signal events: [t, d, sig, val, is_stale] *)
    queue = {},
    done = FALSE,
    processed = 0,
    delta_exceeded = FALSE,

    (* Signal state: array of 4-state values *)
    signals = [i \in 0..(NumSignals-1) |-> "X"],

    (* Phase 2: processes pending to fire on signal changes *)
    pending_processes = {},

    (* Port wire cascade iteration count per delta *)
    cascade_count = 0;

define
    TypeOk ==
        /\ time \in Nat
        /\ delta \in Nat
        /\ queue \subseteq [t: Nat, d: Nat, sig: 0..(NumSignals-1),
                            val: {"0","1","X","Z"}, is_stale: BOOLEAN]
        /\ done \in BOOLEAN
        /\ processed \in Nat
        /\ delta_exceeded \in BOOLEAN
        /\ signals \in [0..(NumSignals-1) -> {"0","1","X","Z"}]
        /\ pending_processes \subseteq 0..(NumSignals-1)
        /\ cascade_count \in 0..16

    NoDeadlock ==
        (queue /= {} /\ ~done) => (processed' > processed)

    DeltaBounded ==
        [] (delta <= MaxDelta)

    EventualAdvance ==
        [] (queue /= {} => <> (time' > time))

    (* A stale event is one scheduled before a direct write.
       No signal should be overwritten by a stale event after
       a process trigger has set it. *)
    NoStaleOverwrite ==
        [](signals /= [i \in 0..(NumSignals-1) |-> "X"] =>
           \A e \in queue : ~(e.is_stale /\ e.t <= time /\ e.d <= delta
                              /\ signals[e.sig] /= "X"))

    (* Port wire cascade must terminate within 16 iterations *)
    CascadeTermination ==
        [](cascade_count <= 16)

    (* An immediate write (non-stale) to a signal should be
       visible to processes in the same delta, not delayed. *)
    ImmediateWriteVisible ==
        [](\A s \in 0..(NumSignals-1) :
            signals[s] /= "X" =>
            ~ \E e \in queue : e.sig = s /\ e.t = time /\ e.d >= delta)
end define;

begin

SchedulerLoop:
    while ~done do
        if queue = {} then
            done := TRUE;
        else
            (* ── Phase 1: Apply all events at current (time, delta) ── *)
            while \E e \in queue : e.t = time /\ e.d = delta do
                with (e \in queue)
                    queue := queue \ {e};
                    if e.is_stale /\ signals[e.sig] /= "X" then
                        (* Stale event: skip (no overwrite) *)
                        skip;
                    else
                        (* Apply the event to the signal *)
                        signals[e.sig] := e.val;
                        processed := processed + 1;

                        (* Check cascade limit *)
                        if cascade_count > 16 then
                            delta_exceeded := TRUE;
                            done := TRUE;
                        end if;

                        (* ── Phase 1.5: Port wire propagation ──
                           INPUT-only: propagate parent (e.sig) to child.
                           Skip OUTPUT (child->parent) unconditionally. *)
                        if e.sig = InputPort then
                            signals[OutputPort] := e.val;
                            cascade_count := cascade_count + 1;
                        end if;

                        (* ── Phase 2: Schedule processes ── *)
                        pending_processes := pending_processes \union {e.sig};
                    end if;
                end with;
            end while;

            (* ── Phase 2b execution ──
               For each pending signal, execute triggered processes.
               This models VHDL CSA immediate write + recursive cascade. *)
            while pending_processes /= {} do
                with (s \in pending_processes)
                    pending_processes := pending_processes \ {s};
                    if s = InputPort then
                        (* INPUT port changed → propagate forward *)
                        signals[OutputPort] := signals[InputPort];
                    end if;
                    if s = OutputPort then
                        (* OUTPUT port changed → propagate backward (Phase 3) *)
                        signals[InputPort] := signals[OutputPort];
                    end if;
                    if cascade_count < 16 then
                        cascade_count := cascade_count + 1;
                        pending_processes := pending_processes \union
                            {if s = InputPort then OutputPort else InputPort};
                    end if;
                end with;
            end while;

            cascade_count := 0;

            (* ── Advance delta or time ── *)
            if \E e \in queue : e.t = time /\ e.d > delta then
                delta := delta + 1;
                if delta > MaxDelta then
                    delta_exceeded := TRUE;
                    done := TRUE;
                end if;
            else
                if \A e \in queue : e.t > time then
                    time := time + 1;
                    delta := 0;
                end if;
            end if;
        end if;
    end while;

end algorithm; *)

====
