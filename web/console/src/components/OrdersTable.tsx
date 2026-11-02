import type { Order } from '../types';
import { fmt } from './OrderBookLadder';

export function OrdersTable({ orders }: { orders: Order[] | undefined }) {
  return (
    <div className="body scroll" data-testid="orders-body">
      <table data-testid="orders-table">
        <thead>
          <tr>
            <th>id</th>
            <th>side</th>
            <th>price</th>
            <th>qty</th>
            <th>filled</th>
            <th>state</th>
          </tr>
        </thead>
        <tbody>
          {orders === undefined ? (
            <tr>
              <td colSpan={6} style={{ color: 'var(--dim)' }}>
                waiting for gateway stream…
              </td>
            </tr>
          ) : orders.length === 0 ? (
            <tr>
              <td colSpan={6} style={{ color: 'var(--dim)' }}>
                no open orders at this event
              </td>
            </tr>
          ) : (
            orders.map((o) => (
              <tr key={o.id} data-testid="order-row">
                <td>{o.id}</td>
                <td style={{ color: o.side === 'B' ? 'var(--bid)' : 'var(--ask)' }}>{o.side}</td>
                <td>{fmt(o.px)}</td>
                <td>{fmt(o.qty)}</td>
                <td>{fmt(o.filled)}</td>
                <td>{o.state}</td>
              </tr>
            ))
          )}
        </tbody>
      </table>
    </div>
  );
}
