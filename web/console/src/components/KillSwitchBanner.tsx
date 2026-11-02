export function KillSwitchBanner({ active }: { active: boolean }) {
  if (!active) return null;
  return (
    <div className="banner" data-testid="kill-banner" role="alert">
      ⚠ KILL SWITCH ENGAGED — new exposure blocked at this event boundary
    </div>
  );
}
